#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <algorithm>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace nix::compact {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

inline void systemFailure(std::string_view operation)
{
    throw std::system_error(errno, std::generic_category(), std::string(operation));
}

struct Descriptor
{
    int fd = -1;
    explicit Descriptor(int fd = -1) : fd(fd) {}
    Descriptor(const Descriptor &) = delete;
    Descriptor & operator=(const Descriptor &) = delete;
    ~Descriptor() { if (fd >= 0) ::close(fd); }
};

inline void writeAll(int fd, std::string_view bytes)
{
    while (!bytes.empty()) {
        auto count = ::write(fd, bytes.data(), bytes.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            systemFailure("writing build log");
        }
        if (count == 0) throw std::runtime_error("zero-length build log write");
        bytes.remove_prefix(static_cast<size_t>(count));
    }
}

inline std::string hex(std::string_view bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        output += digits[c >> 4];
        output += digits[c & 15];
    }
    return output;
}

inline std::string unhex(std::string_view bytes)
{
    if (bytes.size() % 2) throw std::runtime_error("invalid encoded log payload");
    auto digit = [](char c) -> unsigned char {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("invalid encoded log payload");
    };
    std::string result;
    result.reserve(bytes.size() / 2);
    for (size_t i = 0; i < bytes.size(); i += 2)
        result += static_cast<char>((digit(bytes[i]) << 4) | digit(bytes[i + 1]));
    return result;
}

inline std::string timestamp()
{
    auto value = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(value).count());
}

inline std::string quote(std::string_view value)
{
    std::string output = "'";
    for (char c : value) output += c == '\'' ? "'\\''" : std::string(1, c);
    return output + "'";
}

class Archive
{
    Descriptor file;
    Descriptor index;
    uint64_t nextLine = 0;
    uint64_t nextEvent = 0;
    bool finished = false;

public:
    std::filesystem::path path;

    explicit Archive(const std::filesystem::path & directory)
    {
        std::filesystem::create_directories(directory);
        Descriptor dir(::open(directory.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (dir.fd < 0) systemFailure("opening build log directory");
        struct stat info{};
        if (::fstat(dir.fd, &info) < 0) systemFailure("checking build log directory");
        if (info.st_uid != ::geteuid()) throw std::runtime_error("build log directory has a different owner");
        if (::fchmod(dir.fd, 0700) < 0) systemFailure("protecting build log directory");
        // O_EXCL is the collision authority; timestamp and pid are only readable labels.
        std::string stem = timestamp() + "-" + std::to_string(::getpid());
        for (unsigned suffix = 0; suffix < 1000; ++suffix) {
            auto name = stem + "-" + std::to_string(suffix) + ".jsonl";
            file.fd = ::openat(dir.fd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (file.fd >= 0) {
                path = std::filesystem::absolute(directory / name);
                break;
            }
            if (errno != EEXIST) systemFailure("creating build log");
        }
        if (file.fd < 0) throw std::runtime_error("build log name collision limit reached");
        if (::flock(file.fd, LOCK_EX | LOCK_NB) < 0) systemFailure("locking active build log");
        auto indexPath = path.string() + ".index";
        index.fd = ::open(indexPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        append({{"kind", "session"}, {"version", 1}, {"time_ms", timestamp()}});
    }

    uint64_t lines() const { return nextLine; }

    void append(const Json & record)
    {
        auto offset = ::lseek(file.fd, 0, SEEK_CUR);
        writeAll(file.fd, record.dump() + "\n");
        if (index.fd >= 0 && offset >= 0) {
            try {
                auto kind = record.at("kind");
                if (kind == "diagnostic" || kind == "completion"
                    || (kind == "line" && (record.at("line").get<uint64_t>() - 1) % 256 == 0)) {
                    Json entry{{"kind", kind}, {"offset", offset}};
                    if (kind == "line") entry["line"] = record.at("line");
                    if (kind == "diagnostic") entry["id"] = record.at("id");
                    writeAll(index.fd, entry.dump() + "\n");
                }
            } catch (...) {
                // The archive is authoritative. An unavailable acceleration
                // index must not disable capture or make evidence unreadable.
                ::close(index.fd); index.fd = -1;
            }
        }
    }

    template<typename Callback>
    void message(uint64_t activity, std::string_view build, std::string_view payload, Callback callback)
    {
        ++nextEvent;
        // Empty input is a received empty physical line. A trailing newline
        // terminates the preceding line and does not create a phantom extra one.
        do {
            auto end = payload.find('\n');
            bool newline = end != std::string_view::npos;
            auto line = newline ? payload.substr(0, end) : payload;
            Json record{{"kind", "line"}, {"line", ++nextLine}, {"event", nextEvent},
                        {"activity", activity}, {"build", std::string(build)},
                        {"time_ms", timestamp()}, {"newline", newline}};
            // UTF-8 validation is performed by the JSON serializer. Non-UTF-8
            // producer bytes are retained reversibly rather than replaced.
            record["payload"] = std::string(line);
            record["encoding"] = "utf8";
            try {
                (void) record.dump();
            } catch (const Json::type_error &) {
                record["payload"] = hex(line);
                record["encoding"] = "hex";
            }
            append(record);
            callback(nextLine, nextEvent, line);
            if (!newline) break;
            payload.remove_prefix(end + 1);
        } while (!payload.empty());
    }

    void finish(int status)
    {
        if (finished) return;
        append({{"kind", "completion"}, {"status", status}, {"lines", nextLine}, {"time_ms", timestamp()}});
        if (::fsync(file.fd) < 0) systemFailure("syncing completed build log");
        finished = true;
    }
};

struct RetentionPolicy
{
    uint64_t days = 7;
    uint64_t bytes = 1024ULL * 1024 * 1024;
};

inline void pruneCompleted(const std::filesystem::path & directory, RetentionPolicy policy)
{
    struct Candidate { std::filesystem::path path; uint64_t size; std::filesystem::file_time_type modified; };
    std::vector<Candidate> candidates;
    uint64_t total = 0;
    for (const auto & entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".jsonl" || !entry.is_regular_file() || entry.is_symlink()) continue;
        if (candidates.size() >= 10000) break; // Bound work per invocation.
        auto size = entry.file_size();
        total += size;
        candidates.push_back({entry.path(), size, entry.last_write_time()});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto & a, const auto & b) { return a.modified < b.modified; });
    auto now = std::filesystem::file_time_type::clock::now();
    for (const auto & candidate : candidates) {
        bool expired = policy.days && now - candidate.modified > std::chrono::hours(24 * policy.days);
        if (!expired && (!policy.bytes || total <= policy.bytes)) continue;
        Descriptor fd(::open(candidate.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (fd.fd < 0 || ::flock(fd.fd, LOCK_EX | LOCK_NB) < 0) continue;
        struct stat info{};
        if (::fstat(fd.fd, &info) != 0 || info.st_uid != ::geteuid() || !S_ISREG(info.st_mode)) continue;
        // Completion records are small and last. An interrupted session never
        // qualifies for automatic removal, even if its writer is no longer alive.
        auto start = std::max<off_t>(0, info.st_size - 4096);
        if (::lseek(fd.fd, start, SEEK_SET) < 0) continue;
        char buffer[4096];
        auto length = ::read(fd.fd, buffer, sizeof(buffer));
        if (length <= 1 || buffer[length - 1] != '\n') continue;
        std::string_view tail(buffer, static_cast<size_t>(length - 1));
        auto newline = tail.rfind('\n');
        if (newline != std::string_view::npos) tail.remove_prefix(newline + 1);
        auto footer = Json::parse(tail, nullptr, false);
        if (!footer.is_object() || footer.value("kind", "") != "completion") continue;
        if (::unlink(candidate.path.c_str()) == 0) {
            ::unlink((candidate.path.string() + ".index").c_str());
            total -= candidate.size;
        }
    }
}

} // namespace nix::compact
