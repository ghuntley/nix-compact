#pragma once

#include "archive.hh"
#include "classify.hh"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace nix::compact {

struct ContextLine
{
    uint64_t number;
    std::string text;
    bool printed = false;
};

struct BuildContext
{
    std::string name;
    std::deque<ContextLine> preceding;
    size_t following = 0;
    size_t excerptRemaining = 0;
    Clock::time_point followUntil{};
    uint64_t id = 0;
    std::string derivation = {};
    bool hadError = false;
    bool failureTail = false;
    bool heuristicExcerpt = false;
};

inline std::string bounded(std::string_view value, size_t bytes)
{
    if (value.size() <= bytes) return std::string(value);
    constexpr std::string_view marker = " [shortened]";
    if (bytes <= marker.size()) return std::string(value.substr(0, bytes));
    size_t end = bytes - marker.size();
    // Never slice a UTF-8 continuation sequence in valid input.
    while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80) --end;
    return std::string(value.substr(0, end)) + std::string(marker);
}

class Session
{
public:
    using Sink = std::function<void(std::string_view)>;

private:
    Sink sink;
    std::unique_ptr<Archive> archive;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread heartbeat;
    bool stopped = false;
    bool captureFailed = false;
    bool budgetNotice = false;
    bool warningShown = false;
    // Reserve 128 bytes inside the 12 KiB diagnostic budget for the one-time
    // exhaustion notice, even when an excerpt consumes the remaining detail space.
    size_t budget = 12 * 1024 - 128;
    size_t heuristicBudget = 4 * 1024;
    uint64_t diagnostics = 0;
    uint64_t omitted = 0;
    uint64_t repeats = 0;
    std::map<uint64_t, BuildContext> contexts;
    std::deque<BuildContext> completed;
    std::set<std::pair<uint64_t, std::string>> seen;
    Clock::time_point started = Clock::now();
    std::string progress = "working";
    std::string location;
    std::string finalError;

    void emit(std::string_view value) noexcept
    {
        try { sink(value); } catch (...) { /* Logging must never change a build result. */ }
    }

    void failCapture() noexcept
    {
        if (captureFailed) return;
        captureFailed = true;
        emit("[nix] Build log capture failed; restoring visible output. Archive may be incomplete.\n");
    }

    bool detail(std::string_view value, size_t & excerptRemaining, bool heuristic = false)
    {
        size_t available = std::min(budget, excerptRemaining);
        if (heuristic) available = std::min(available, heuristicBudget);
        if (available < 32) return false;
        auto output = bounded(value, available - 1) + "\n";
        budget -= output.size();
        excerptRemaining -= output.size();
        if (heuristic) heuristicBudget -= output.size();
        emit(output);
        return true;
    }

    static std::string display(const BuildContext & context, const ContextLine & line)
    {
        return "L" + std::to_string(line.number) + " [" + context.name + "] " + line.text;
    }

    void received(uint64_t activity, uint64_t number, uint64_t event,
                  std::string_view payload, std::optional<Severity> native, std::string_view primaryMessage = {},
                  std::string_view nativeEvent = {})
    {
        // Limit live context independently of unbounded archive length. Unknown
        // activities beyond the cap still archive and classify, without a tail.
        BuildContext temporary{"unattributed", {}, 0, 0, {}};
        auto found = contexts.find(activity);
        BuildContext & context = found == contexts.end() ? temporary : found->second;
        ContextLine line{number, bounded(plain(payload.substr(0, 8192)), 2048), false};
        auto diagnostic = classify(payload);
        if (native) diagnostic = {*native, Confidence::native, "nix", std::string(primaryMessage)};
        if (diagnostic.severity != Severity::routine) {
            context.following = 0;
            ++diagnostics;
            const auto id = diagnostics;
            auto dedupContent = native ? nativeEvent : payload;
            auto fingerprint = std::make_pair(activity, dedupContent.size() <= 2048 ? std::string(dedupContent) : "");
            // Only exact, fully retained lines are deduplicated. Long payloads
            // sharing a prefix are not asserted to be duplicates.
            bool repeat = dedupContent.size() <= 2048 && seen.contains(fingerprint);
            if (dedupContent.size() <= 2048 && seen.size() < 1024) seen.insert(std::move(fingerprint));
            archive->append({{"kind", "diagnostic"}, {"id", id}, {"line", number}, {"event", event},
                {"activity", activity}, {"build", context.name}, {"source", diagnostic.source},
                {"severity", diagnostic.severity == Severity::error ? "error" : "warning"},
                {"repeat", repeat}, {"summary", bounded(plain((primaryMessage.empty() ? payload : primaryMessage).substr(0, 1024)), 240)}});
            if (repeat) ++repeats;
            bool showWarning = diagnostic.severity != Severity::warning || !warningShown;
            bool heuristic = diagnostic.confidence == Confidence::heuristic;
            if (repeat || !showWarning || budget < 256 || (heuristic && heuristicBudget < 256)) {
                ++omitted;
            } else {
                warningShown |= diagnostic.severity == Severity::warning;
                context.following = diagnostic.severity == Severity::error ? 20 : 0;
                context.followUntil = Clock::now() + std::chrono::seconds(2);
                context.excerptRemaining = std::min<size_t>(4096, budget);
                context.heuristicExcerpt = heuristic;
                if (heuristic) context.excerptRemaining = std::min(context.excerptRemaining, heuristicBudget);
                auto header = "[nix] D" + std::to_string(id) + " — " + context.name + " — "
                    + diagnostic.source + "/" + (diagnostic.severity == Severity::error ? "error" : "warning");
                detail(header, context.excerptRemaining, heuristic);
                // Reserve the primary diagnostic before spending space on its
                // context. Nearby context is selected newest-first, printed in order.
                auto primary = diagnostic.rendered.empty() ? display(context, line)
                    : "L" + std::to_string(line.number) + " [" + context.name + "] [rendered] "
                        + bounded(plain(diagnostic.rendered), 2048);
                size_t reserve = std::min<size_t>(primary.size() + 1, 2048);
                size_t available = context.excerptRemaining > reserve ? context.excerptRemaining - reserve : 0;
                size_t start = context.preceding.size();
                size_t count = context.failureTail && !context.hadError ? 40 : 20;
                size_t lower = start > count ? start - count : 0;
                while (start > lower) {
                    auto & previous = context.preceding[start - 1];
                    size_t cost = previous.printed ? 0 : display(context, previous).size() + 1;
                    if (cost > available) break;
                    available -= cost;
                    --start;
                }
                if (diagnostic.severity == Severity::error) {
                    for (; start < context.preceding.size(); ++start) {
                        auto & previous = context.preceding[start];
                        if (!previous.printed) {
                            previous.printed = detail(display(context, previous), context.excerptRemaining, heuristic);
                        }
                    }
                }
                line.printed = detail(primary, context.excerptRemaining, heuristic);
            }
            context.hadError |= diagnostic.severity == Severity::error;
            context.failureTail = false;
            if (budget < 256 && !budgetNotice) {
                budgetNotice = true;
                emit("[nix] Diagnostic budget reached; further details saved to the advertised log (use --diagnostics).\n");
            }
        } else if (context.following && Clock::now() <= context.followUntil) {
            --context.following;
            line.printed = detail(display(context, line), context.excerptRemaining, context.heuristicExcerpt);
        } else context.following = 0;
        if (found != contexts.end()) {
            context.preceding.push_back(std::move(line));
            if (context.preceding.size() > 40) context.preceding.pop_front();
        }
    }

public:
    Session(const std::filesystem::path & directory, Sink sink, RetentionPolicy retention = {}) : sink(std::move(sink))
    {
        try {
            archive = std::make_unique<Archive>(directory);
            location = archive->path.string();
            try { pruneCompleted(directory, retention); } catch (...) {
                emit("[nix] Log retention could not complete; existing archives retained.\n");
            }
            contexts.emplace(0, BuildContext{"nix", {}, 0, 0, {}});
            emit("[nix] Output suppressed; progress every 15s. Full log: " + location + "\n");
            emit("[nix] L# = log line; D# = diagnostic. Read: nix build-log " + quote(location) + " --diagnostics\n");
            heartbeat = std::thread([this] {
                std::unique_lock lock(mutex);
                while (!wake.wait_for(lock, std::chrono::seconds(15), [this] { return stopped; })) {
                    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count();
                    auto line = "[nix] " + std::to_string(seconds) + "s: " + progress;
                    if (omitted) line += "; " + std::to_string(omitted) + " diagnostic candidates saved";
                    emit(bounded(line, 159) + "\n");
                }
            });
        } catch (...) { failCapture(); }
    }

    ~Session() { stop(); }

    void stop() noexcept
    {
        {
            std::lock_guard lock(mutex);
            stopped = true;
        }
        wake.notify_all();
        if (heartbeat.joinable()) heartbeat.join();
    }

    void activity(uint64_t id, std::string_view name, std::string_view derivation = {}) noexcept
    {
        try {
            std::lock_guard lock(mutex);
            if (contexts.size() < 256) {
                auto [it, added] = contexts.try_emplace(id, BuildContext{bounded(plain(name), 100), {}, 0, 0, {}});
                if (added) { it->second.id = id; it->second.derivation = derivation; }
            }
        } catch (...) { /* Context is best effort; archival capture remains independent. */ }
    }

    void endActivity(uint64_t id) noexcept
    {
        try {
            std::lock_guard lock(mutex);
            auto found = contexts.find(id);
            if (id && found != contexts.end()) {
                if (!found->second.derivation.empty()) {
                    completed.push_back(std::move(found->second));
                    if (completed.size() > 16) completed.pop_front();
                }
                contexts.erase(found);
            }
        } catch (...) {}
    }

    void status(std::string_view value) noexcept
    {
        try {
            std::lock_guard lock(mutex);
            progress = bounded(plain(value), 100);
            std::replace(progress.begin(), progress.end(), '\n', ' ');
            std::replace(progress.begin(), progress.end(), '\t', ' ');
        } catch (...) {}
    }

    void message(uint64_t activity, std::string_view payload, std::optional<Severity> native = {},
                 std::string_view primaryMessage = {}) noexcept
    {
        std::lock_guard lock(mutex);
        if (captureFailed) { emit(payload); emit("\n"); return; }
        try {
            if (native == Severity::error && activity == 0) {
                // Nix ErrorInfo names the exact derivation. Re-associate its
                // terminal failure with recently stopped builder context; never
                // guess from a short application/build name.
                for (auto it = completed.rbegin(); it != completed.rend(); ++it) {
                    if (!it->derivation.empty() && payload.find(it->derivation) != std::string_view::npos) {
                        activity = it->id;
                        it->failureTail = true;
                        if (contexts.size() < 256) contexts.try_emplace(activity, *it);
                        break;
                    }
                }
            }
            auto found = contexts.find(activity);
            std::string name = found == contexts.end() ? "unattributed" : found->second.name;
            if (native == Severity::error)
                finalError = bounded(plain((primaryMessage.empty() ? payload : primaryMessage).substr(0, 8192)), 1400);
            bool first = true;
            archive->message(activity, name, payload,
                [&](uint64_t line, uint64_t event, std::string_view part) {
                    received(activity, line, event, part, native && !first ? std::optional{Severity::routine} : native,
                        first ? primaryMessage : std::string_view{}, payload);
                    first = false;
                });
        } catch (...) {
            failCapture();
            emit(payload);
            emit("\n");
        }
    }

    void finish(int result) noexcept
    {
        stop();
        std::lock_guard lock(mutex);
        try { if (!captureFailed) archive->finish(result); }
        catch (...) { failCapture(); }
        try {
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count();
            std::string summary = std::string("[nix] ") + (result == 0 ? "SUCCESS" : "FAILED")
                + " in " + std::to_string(seconds) + "s | exit " + std::to_string(result)
                + " | " + std::to_string(diagnostics) + " diagnostic candidates; "
                + std::to_string(omitted) + " omitted; " + std::to_string(repeats) + " repeats\n";
            std::string footer;
            if (!location.empty()) {
                footer = "[nix] " + std::string(captureFailed ? "Partial log: " : "Log: ") + location + "\n";
                if (result != 0) footer += "[nix] Inspect: nix build-log " + quote(location) + " --diagnostics --limit 3\n";
            }
            if (summary.size() + footer.size() > 2048)
                footer = "[nix] Full log path and reader command were advertised at session start.\n";
            constexpr std::string_view label = "[nix] Final Nix failure (reserved summary):\n";
            size_t available = 2048 - summary.size() - footer.size();
            if (result != 0 && !finalError.empty() && omitted && available > label.size() + 32)
                summary += std::string(label) + bounded(finalError, available - label.size() - 1) + "\n";
            emit(summary + footer);
        } catch (...) { failCapture(); }
    }
};

} // namespace nix::compact
