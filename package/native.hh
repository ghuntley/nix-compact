#pragma once

#include "session.hh"
#include "reader.hh"

#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/logging.hh"

namespace nix::compact {

inline std::shared_ptr<Session> currentSession;

class CompactLogger final : public Logger
{
    std::shared_ptr<Session> session;
    std::unique_ptr<Logger> previous;
    std::mutex progressMutex;
    std::map<ActivityId, ActivityType> types;
    uint64_t built = 0, running = 0, copied = 0;
    std::string phase;

    static std::string field(const Fields & fields, size_t index)
    {
        return index < fields.size() && fields[index].type == Field::tString ? fields[index].s : "";
    }

public:
    CompactLogger(std::shared_ptr<Session> session, std::unique_ptr<Logger> previous)
        : session(std::move(session)), previous(std::move(previous))
    {
        this->previous->stop();
    }

    // Upstream uses this to decide whether to attach a failed builder's tail
    // to its final ErrorInfo. Keep the tail: console streaming is suppressed.
    bool isVerbose() override { return false; }

    void log(Verbosity level, std::string_view message) override
    {
        // Nix also uses lvlError for mandatory non-error output such as the
        // "these derivations will be built" plan. ErrorInfo below is authoritative;
        // unstructured lvlError strings still need diagnostic classification.
        if (level == lvlError) session->message(0, message);
        else session->message(0, message, level == lvlWarn ? Severity::warning : Severity::routine);
    }

    void logEI(const ErrorInfo & error) override
    {
        std::ostringstream output;
        showErrorInfo(output, error, loggerSettings.showTrace.get());
        session->message(0, output.str(), error.level == lvlError ? Severity::error
            : error.level == lvlWarn ? Severity::warning : Severity::routine, error.msg.str());
    }

    void startActivity(ActivityId id, Verbosity, ActivityType type, const std::string & message,
                       const Fields & fields, ActivityId) override
    {
        {
            std::lock_guard lock(progressMutex);
            if (types.size() < 4096) types[id] = type;
        }
        if (type == actBuild || type == actPostBuildHook) {
            auto name = std::filesystem::path(field(fields, 0)).filename().string();
            auto dash = name.find('-');
            if (dash != std::string::npos) name.erase(0, dash + 1);
            if (name.ends_with(".drv")) name.resize(name.size() - 4);
            session->activity(id, name.empty() ? "unnamed" : name, field(fields, 0));
        }
        if (!message.empty()) session->message(id, message, Severity::routine);
    }

    void stopActivity(ActivityId id) override
    {
        { std::lock_guard lock(progressMutex); types.erase(id); }
        session->endActivity(id);
    }

    void result(ActivityId id, ResultType type, const Fields & fields) override
    {
        if (type == resBuildLogLine || type == resPostBuildLogLine) session->message(id, field(fields, 0));
        else if (type == resSetPhase || type == resProgress) {
            std::lock_guard lock(progressMutex);
            if (type == resSetPhase) phase = bounded(plain(field(fields, 0)), 40);
            else if (fields.size() >= 4 && fields[0].type == Field::tInt && fields[2].type == Field::tInt) {
                auto activity = types.find(id);
                if (activity == types.end()) return;
                if (activity->second == actBuilds) { built = fields[0].i; running = fields[2].i; }
                else if (activity->second == actCopyPaths) copied = fields[0].i;
                else return;
            }
            session->status(std::to_string(built) + " built, " + std::to_string(running)
                + " running, " + std::to_string(copied) + " copied" + (phase.empty() ? "" : "; phase: " + phase));
        }
    }

    void stop() override { session->stop(); }
    std::optional<char> ask(std::string_view message) override
    {
        if (!nix::isTTY()) return {};
        writeToStderr(std::string(message) + " ");
        auto answer = trim(readLine(getStandardInput(), true));
        return answer.size() == 1 ? std::optional<char>{answer[0]} : std::nullopt;
    }
};

inline void start()
{
    std::filesystem::path directory;
    if (auto value = getEnv("NIX_BUILD_LOG_DIR")) directory = *value;
    else if (auto state = getEnv("XDG_STATE_HOME"); state && std::filesystem::path(*state).is_absolute())
        directory = std::filesystem::path(*state) / "nix/build-logs";
    else if (auto home = getEnv("HOME")) directory = std::filesystem::path(*home) / ".local/state/nix/build-logs";
    else {
        writeToStderr("[nix] No build-log directory available; retaining visible output.\n");
        settings.verboseBuild = true;
        logger->setPrintBuildLogs(true);
        return;
    }
    RetentionPolicy retention;
    try {
        if (auto value = getEnv("NIX_BUILD_LOG_KEEP_DAYS")) retention.days = number(*value);
        if (auto value = getEnv("NIX_BUILD_LOG_MAX_BYTES")) retention.bytes = number(*value);
        if (retention.days > 36500) throw std::runtime_error("retention days exceeds 100 years");
    } catch (...) {
        writeToStderr("[nix] Invalid log retention setting; using 7 days and 1 GiB.\n");
        retention = {};
    }
    // Stop the upstream terminal redraw before printing the suppression notice.
    logger->stop();
    currentSession = std::make_shared<Session>(directory, [](std::string_view output) { writeToStderr(output); }, retention);
    // Request all builder lines using the existing daemon protocol. Filtering
    // happens only after capture; this does not change worker verbosity itself.
    settings.verboseBuild = true;
    logger = std::make_unique<CompactLogger>(currentSession, std::move(logger));
}

inline void finish(int result) noexcept
{
    if (currentSession) currentSession->finish(result);
}

struct CmdBuildLog : Command
{
    std::string path;
    std::vector<std::string> options;

    CmdBuildLog()
    {
        expectArgs({.label = "log", .handler = {&path}});
        for (const std::string flag : {"lines", "from-line", "limit", "context", "byte-offset", "tail", "after", "diagnostic", "build", "activity", "format"}) {
            addFlag({.longName = flag, .description = "Select bounded build-log output (see command help).",
                .labels = {"value"}, .handler = {[this, flag](std::string value) {
                    options.push_back("--" + flag); options.push_back(std::move(value));
                }}});
        }
        for (const std::string flag : {"diagnostics", "all"}) {
            addFlag({.longName = flag, .description = flag == "all" ? "Explicitly remove output limits." : "List diagnostic references.",
                .handler = {[this, flag]() { options.push_back("--" + flag); }}});
        }
    }

    std::string description() override { return "read bounded, numbered evidence from a compact build session"; }

    std::string doc() override
    {
        return R"(
# Description

Read a private compact build archive. L identifiers refer to received physical
log lines; D identifiers refer to diagnostic candidates, not build failures.
Output defaults to 100 lines and 8 KiB. Continuation commands preserve selection.

Use `--diagnostics` to list candidates, `--diagnostic D1 --context 40` for an
activity-scoped excerpt, `--lines 100:200`, `--from-line 100`, or `--tail 100`.
`--build NAME` filters build identity; global line identifiers retain gaps.
`--limit N` limits rows, `--after D1` pages diagnostic summaries, and
`--byte-offset N` resumes within an oversized rendered line. `--format original`
returns received payload bytes without line labels. `--all` explicitly removes
output budgets. A session without a completion footer is incomplete or running.
)";
    }

    void run() override
    {
        options.insert(options.begin(), path);
        try {
            logger->stop();
            readLog(readOptions(options),
                [](std::string_view output) { writeFull(getStandardOutput(), output); },
                [](std::string_view output) { writeToStderr(output); });
        } catch (const std::exception & error) {
            throw Error("build-log: %s", error.what());
        }
    }
};

inline auto registeredBuildLog = registerCommand<CmdBuildLog>("build-log");

} // namespace nix::compact
