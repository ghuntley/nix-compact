#pragma once

#include "archive.hh"
#include "classify.hh"

#include <charconv>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace nix::compact {

struct ReadOptions
{
    std::filesystem::path path;
    uint64_t first = 1;
    uint64_t last = std::numeric_limits<uint64_t>::max();
    size_t limit = 100;
    size_t bytes = 8192;
    size_t context = 20;
    size_t offset = 0;
    size_t tail = 0;
    uint64_t diagnostic = 0;
    uint64_t after = 0;
    std::optional<uint64_t> activity;
    std::string build;
    bool diagnostics = false;
    bool original = false;
    bool all = false;
};

inline uint64_t number(std::string_view value)
{
    uint64_t result = 0;
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("expected an unsigned decimal number");
    return result;
}

inline uint64_t diagnosticNumber(std::string_view value)
{
    if (value.starts_with('D')) value.remove_prefix(1);
    auto result = number(value);
    if (!result) throw std::runtime_error("diagnostic identifiers start at D1");
    return result;
}

inline ReadOptions readOptions(const std::vector<std::string> & args)
{
    if (args.empty()) throw std::runtime_error("usage: nix build-log LOG [--diagnostics | --diagnostic D1 | --lines 1:100] [--build NAME] [--limit 100] [--format original] [--all]");
    ReadOptions options;
    options.path = std::filesystem::absolute(args[0]);
    for (size_t i = 1; i < args.size(); ++i) {
        const auto & key = args[i];
        if (key == "--all") { options.all = true; continue; }
        if (key == "--diagnostics") { options.diagnostics = true; continue; }
        if (++i == args.size()) throw std::runtime_error("missing build-log option value");
        const auto & value = args[i];
        if (key == "--lines") {
            auto colon = value.find(':');
            if (colon == std::string::npos) throw std::runtime_error("--lines requires FIRST:LAST");
            options.first = number(std::string_view(value).substr(0, colon));
            options.last = number(std::string_view(value).substr(colon + 1));
        } else if (key == "--from-line") options.first = number(value);
        else if (key == "--limit") options.limit = number(value);
        else if (key == "--context") options.context = number(value);
        else if (key == "--byte-offset") options.offset = number(value);
        else if (key == "--tail") options.tail = number(value);
        else if (key == "--after") options.after = diagnosticNumber(value);
        else if (key == "--diagnostic") options.diagnostic = diagnosticNumber(value);
        else if (key == "--build") options.build = value;
        else if (key == "--activity") options.activity = number(value);
        else if (key == "--format" && value == "original") options.original = true;
        else throw std::runtime_error("unknown build-log option or format");
    }
    if (!options.first || options.last < options.first || !options.limit
        || options.limit > 10000 || options.context > 10000 || options.tail > 10000)
        throw std::runtime_error("invalid range; limit, tail and context may not exceed 10000");
    if (options.diagnostics && (options.diagnostic || options.tail || options.original || options.offset))
        throw std::runtime_error("diagnostic index cannot be combined with context, tail, original format or byte offset");
    if (options.diagnostic && options.tail) throw std::runtime_error("choose diagnostic or tail selection");
    return options;
}

inline bool readRecord(std::istream & stream, Json & record)
{
    // The parser is bounded even for a corrupt/manually edited archive. A
    // larger physical record remains in the file for external streaming tools.
    constexpr size_t maximum = 64 * 1024 * 1024;
    std::string line;
    char c;
    while (stream.get(c)) {
        if (c == '\n') break;
        if (line.size() == maximum) throw std::runtime_error("archive record exceeds 64 MiB reader limit; inspect the archive directly");
        line += c;
    }
    if (line.empty() && !stream) return false;
    if (!stream && !line.empty()) return false; // interrupted final append
    if (!shallow(line)) throw std::runtime_error("archive record nesting exceeds reader limit");
    record = Json::parse(line, nullptr, false);
    if (!record.is_object() || !member(record, "kind") || !record["kind"].is_string())
        throw std::runtime_error("invalid build-log record");
    return true;
}

inline uint64_t recordNumber(const Json & record, std::string_view field)
{
    auto value = member(record, field);
    if (!value || !value->is_number_unsigned()) throw std::runtime_error("invalid numeric build-log field");
    return value->get<uint64_t>();
}

inline std::string payload(const Json & record)
{
    auto data = member(record, "payload");
    if (!data || !data->is_string()) throw std::runtime_error("invalid build-log payload");
    auto encoding = text(record, "encoding");
    if (encoding == "hex") return unhex(data->get_ref<const std::string &>());
    if (encoding == "utf8") return data->get<std::string>();
    throw std::runtime_error("unsupported build-log payload encoding");
}

inline std::streamoff indexedOffset(const std::filesystem::path & path, uint64_t first)
{
    try {
        std::ifstream index(path.string() + ".index", std::ios::binary);
        Json entry;
        std::streamoff best = 0;
        // Bound optional-index work; malformed, missing or stale entries fall
        // back to scanning the authoritative archive, never to missing data.
        size_t count = 0;
        while (index && count++ < 100000 && readRecord(index, entry)) {
            if (text(entry, "kind") != "line" || recordNumber(entry, "line") > first) continue;
            auto offset = recordNumber(entry, "offset");
            if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) continue;
            std::ifstream archive(path, std::ios::binary);
            archive.seekg(static_cast<std::streamoff>(offset));
            Json actual;
            if (readRecord(archive, actual) && text(actual, "kind") == "line"
                && recordNumber(actual, "line") == recordNumber(entry, "line"))
                best = std::max(best, static_cast<std::streamoff>(offset));
        }
        return best;
    } catch (...) { return 0; }
}

inline int readLog(ReadOptions options, const std::function<void(std::string_view)> & output,
                   const std::function<void(std::string_view)> & notice)
{
    std::ifstream file(options.path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open build log");
    Json record;
    if (!readRecord(file, record) || text(record, "kind") != "session" || recordNumber(record, "version") != 1)
        throw std::runtime_error("unsupported build-log archive version");

    std::optional<uint64_t> activity = options.activity;
    std::string completion = "INCOMPLETE (or still running)";
    std::deque<uint64_t> tail;
    std::optional<uint64_t> diagnosticLine;
    // Ordinary range reads need only the small completion footer. Diagnostic
    // and tail selection perform a bounded-memory pass over the archive.
    if (!options.diagnostic && !options.tail) {
        file.seekg(0, std::ios::end);
        auto end = file.tellg();
        auto start = std::max<std::streamoff>(0, static_cast<std::streamoff>(end) - 4096);
        file.seekg(start);
        if (start) { std::string discard; std::getline(file, discard); }
    }
    // Selection pass retains only bounded identities, never all payloads.
    while (readRecord(file, record)) {
        auto kind = text(record, "kind");
        if (kind == "completion") {
            auto status = recordNumber(record, "status");
            if (status > 255) throw std::runtime_error("invalid completion status");
            completion = status == 0 ? "SUCCESS" : "FAILED";
        }
        if (kind == "diagnostic" && options.diagnostic && recordNumber(record, "id") == options.diagnostic) {
            activity = recordNumber(record, "activity");
            diagnosticLine = recordNumber(record, "line");
        }
        if (kind == "line" && options.tail && (options.build.empty() || text(record, "build") == options.build)
            && (!activity || recordNumber(record, "activity") == *activity)) {
            tail.push_back(recordNumber(record, "line"));
            if (tail.size() > options.tail) tail.pop_front();
        }
    }
    if (options.diagnostic && !diagnosticLine) throw std::runtime_error("diagnostic not found in this session");
    if (options.tail && !tail.empty()) options.first = tail.front();

    if (diagnosticLine) {
        // Count preceding/following physical lines within the actual activity,
        // rather than letting interleaved builds consume the context allowance.
        file.clear(); file.seekg(0);
        std::deque<uint64_t> before;
        size_t after = 0;
        options.first = *diagnosticLine;
        options.last = *diagnosticLine;
        while (readRecord(file, record)) {
            if (text(record, "kind") != "line" || recordNumber(record, "activity") != *activity) continue;
            auto line = recordNumber(record, "line");
            if (line < *diagnosticLine) {
                before.push_back(line);
                if (before.size() > options.context) before.pop_front();
            } else if (line > *diagnosticLine && after++ < options.context) options.last = line;
        }
        if (!before.empty()) options.first = before.front();
    }

    notice("[nix] Session: " + completion + ". L# = saved log line; D# = diagnostic.\n");
    if (activity || !options.build.empty()) notice("[nix] Build filter active; line-number gaps retain other activity in the archive.\n");
    const std::string command = "nix build-log " + quote(options.path.string());
    size_t emitted = 0, bytes = 0;
    uint64_t resumeLine = 0, resumeAfter = options.after;
    size_t resumeOffset = 0;
    bool truncated = false;
    file.clear(); file.seekg(0);
    if (!options.diagnostics && options.first > 1) file.seekg(indexedOffset(options.path, options.first));
    while (readRecord(file, record)) {
        if (options.diagnostics) {
            if (text(record, "kind") != "diagnostic") continue;
            auto id = recordNumber(record, "id");
            if (id <= options.after || (!options.build.empty() && text(record, "build") != options.build)) continue;
            std::string row = "D" + std::to_string(id) + " L" + std::to_string(recordNumber(record, "line"))
                + " [" + plain(text(record, "build")) + "] " + text(record, "source") + "/"
                + text(record, "severity") + " " + plain(text(record, "summary")) + "\n";
            if (!options.all && (emitted >= options.limit || bytes + row.size() > options.bytes)) { truncated = true; break; }
            output(row); bytes += row.size(); ++emitted; resumeAfter = id;
            continue;
        }
        if (text(record, "kind") != "line") continue;
        auto line = recordNumber(record, "line");
        if (line < options.first || line > options.last) continue;
        if (activity && recordNumber(record, "activity") != *activity) continue;
        if (!options.build.empty() && text(record, "build") != options.build) continue;
        auto original = payload(record);
        std::string value = options.original ? original : plain(original);
        size_t skip = line == options.first ? options.offset : 0;
        if (skip > value.size()) throw std::runtime_error("byte offset exceeds selected rendered line");
        std::string prefix = options.original ? "" : "L" + std::to_string(line) + " [" + plain(text(record, "build")) + "] ";
        auto newline = member(record, "newline");
        if (!newline || !newline->is_boolean()) throw std::runtime_error("invalid line terminator field");
        std::string ending = !options.original || newline->get<bool>() ? "\n" : "";
        size_t available = options.all ? std::numeric_limits<size_t>::max() : options.bytes - bytes;
        if (!options.all && (emitted >= options.limit || available <= prefix.size() + ending.size())) {
            truncated = true; resumeLine = line; resumeOffset = skip; break;
        }
        size_t length = std::min(value.size() - skip, available - prefix.size() - ending.size());
        if (!options.original && skip + length < value.size()) {
            while (length && (static_cast<unsigned char>(value[skip + length]) & 0xc0) == 0x80) --length;
        }
        // Offsets refer to the chosen rendering (original bytes or normalized
        // text), so continuation does not silently skip terminal escape bytes.
        output(prefix); output(std::string_view(value).substr(skip, length));
        if (length == value.size() - skip) output(ending);
        bytes += prefix.size() + length + (length == value.size() - skip ? ending.size() : 0);
        ++emitted;
        if (length < value.size() - skip) {
            truncated = true; resumeLine = line; resumeOffset = skip + length; break;
        }
    }
    if (truncated) {
        std::string next = command;
        if (options.diagnostics) next += " --diagnostics --after D" + std::to_string(resumeAfter);
        else {
            next += " --lines " + std::to_string(resumeLine) + ":" + std::to_string(options.last);
            if (resumeOffset) next += " --byte-offset " + std::to_string(resumeOffset);
            if (options.original) next += " --format original";
            // Diagnostic continuation must retain the exact activity, not a
            // potentially repeated build name. Internal --activity is added below.
            if (activity) next += " --activity " + std::to_string(*activity);
        }
        if (!options.build.empty()) next += " --build " + quote(options.build);
        next += " --limit " + std::to_string(options.limit);
        notice("\n[nix] Output limit reached. Continue: " + next + "\n");
    } else notice("[nix] End of selected output.\n");
    return 0;
}

} // namespace nix::compact
