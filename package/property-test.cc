#include "session.hh"
#include "reader.hh"
#include <hegel.h>

#include <cstdlib>
#include <iostream>
#include <vector>
#include <sys/wait.h>
#include <sys/resource.h>
#include <csignal>
#include <pty.h>

using namespace nix::compact;

struct DrawStopped {};

class Hegel
{
public:
    hegel_context_t * context = hegel_context_new();
    Hegel() { if (!context) throw std::runtime_error("Hegel context allocation failed"); }
    ~Hegel() { hegel_context_free(context); }
    Hegel(const Hegel &) = delete;
    Hegel & operator=(const Hegel &) = delete;

    void check(hegel_result_t result) const
    {
        if (result == HEGEL_E_STOP_TEST) throw DrawStopped{};
        if (result != HEGEL_OK) {
            const char * error = hegel_context_last_error(context);
            throw std::runtime_error(error ? error : "Hegel API failure");
        }
    }

    int64_t integer(hegel_test_case_t * tc, int64_t min, int64_t max) const
    {
        int64_t value = 0;
        check(hegel_generate_integer(context, tc, min, max, &value));
        return value;
    }

    std::string bytes(hegel_test_case_t * tc, uint64_t max) const
    {
        hegel_generate_bytes_result_t result{};
        check(hegel_generate_bytes(context, tc, 0, max, &result));
        std::string value(reinterpret_cast<const char *>(result.data), result.len);
        check(hegel_generate_bytes_result_free(context, &result));
        return value;
    }
};

static void require(bool condition, const char * invariant)
{
    if (!condition) throw std::runtime_error(invariant);
}

static std::vector<Json> records(const std::filesystem::path & path)
{
    std::ifstream file(path);
    require(file.good(), "archive must be readable");
    std::vector<Json> result;
    std::string line;
    while (std::getline(file, line)) result.push_back(Json::parse(line));
    return result;
}

static void roundTrip(const Hegel & h, hegel_test_case_t * tc, const std::filesystem::path & directory)
{
    auto payload = h.bytes(tc, 4096);
    auto status = h.integer(tc, 0, 255);
    std::filesystem::path path;
    {
        Archive archive(directory);
        path = archive.path;
        archive.message(7, "fixture", payload, [](uint64_t, uint64_t, std::string_view) {});
        archive.finish(static_cast<int>(status));
    }
    std::string restored;
    uint64_t expected = 1;
    bool completed = false;
    for (const auto & record : records(path)) {
        if (record.at("kind") == "line") {
            require(record.at("line") == expected++, "line identifiers must be contiguous and stable");
            require(record.at("activity") == 7, "activity identity must survive capture");
            auto data = record.at("payload").get<std::string>();
            restored += record.at("encoding") == "hex" ? unhex(data) : data;
            if (record.at("newline").get<bool>()) restored += '\n';
        } else if (record.at("kind") == "completion") {
            require(record.at("status") == status, "completion must preserve the supplied result");
            completed = true;
        }
    }
    require(completed && restored == payload, "arbitrary bytes must round-trip exactly");
    std::filesystem::remove(path);
}

static void structuredPrecedence(const Hegel & h, hegel_test_case_t * tc, const std::filesystem::path &)
{
    // Convert arbitrary bytes to printable text; inject error words regardless
    // of payload generation so precedence is exercised in every example.
    auto payload = hex(h.bytes(tc, 1024)) + " ERROR failed fatal panic warning";
    auto variant = h.integer(tc, 0, 3);
    Json object;
    if (variant == 0) object = {{"severity", "info"}, {"message", payload}};
    else if (variant == 1) object = {{"level", "INFO"}, {"fields", {{"message", payload}, {"level", "ERROR"}}}};
    else if (variant == 2) object = {{"level", "debug"}, {"message", payload}};
    else object = {{"reason", "compiler-artifact"}, {"target", {{"name", payload}}}};
    auto result = classify(object.dump());
    require(result.severity == Severity::routine && result.confidence == Confidence::structured,
            "structured non-error severity must defeat arbitrary error-like payload words");
    // Arbitrary malformed bytes must not throw or change classification state.
    (void) classify(h.bytes(tc, 4096));
    require(classify(object.dump()).severity == Severity::routine, "classifier must be stateless");
}

static void budgetsAndInterleaving(const Hegel & h, hegel_test_case_t * tc, const std::filesystem::path & directory)
{
    auto count = h.integer(tc, 1, 180);
    auto width = h.integer(tc, 0, 3000);
    auto result = h.integer(tc, 0, 1);
    std::vector<uint64_t> owners;
    for (int64_t i = 0; i < count; ++i) owners.push_back(static_cast<uint64_t>(h.integer(tc, 1, 3)));
    std::string console;
    {
        Session session(directory, [&](std::string_view output) { console += output; });
        for (uint64_t id = 1; id <= 3; ++id) session.activity(id, "build-" + std::to_string(id));
        for (size_t i = 0; i < owners.size(); ++i) {
            auto id = owners[i];
            std::string payload = "ERROR owner=" + std::to_string(id) + " event=" + std::to_string(i)
                + " " + std::string(static_cast<size_t>(width), 'x');
            session.message(id, payload);
        }
        session.finish(static_cast<int>(result));
    }
    // Notices have bounded path-dependent size and are separate from details.
    auto start = console.find("[nix] D1");
    auto finish = console.find(result == 0 ? "[nix] SUCCESS" : "[nix] FAILED");
    require(start != std::string::npos && finish != std::string::npos, "diagnostic and actual result must be visible");
    require(finish - start <= 12 * 1024, "automatic diagnostic bytes must obey invocation budget");
    std::istringstream rendered(console);
    std::string line;
    std::set<uint64_t> displayed;
    while (std::getline(rendered, line)) {
        if (!line.starts_with('L')) continue;
        auto separator = line.find(' ');
        auto number = std::stoull(line.substr(1, separator - 1));
        require(displayed.insert(number).second, "overlapping context must not be printed twice");
        require(number >= 1 && number <= owners.size(), "displayed reference must address an archived line");
        auto owner = std::to_string(owners[number - 1]);
        require(line.find("[build-" + owner + "]") != std::string::npos, "context must retain correct build identity");
        require(line.find("owner=" + owner) != std::string::npos, "context payload must match activity identity");
    }
    for (const auto & entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".jsonl") continue;
        auto log = records(entry.path());
        uint64_t captured = 0;
        for (const auto & record : log) if (record.at("kind") == "line") ++captured;
        require(captured == owners.size(), "console suppression must not drop archived lines");
        require(log.back().at("status") == result, "error words must not change actual result");
        std::filesystem::remove(entry.path());
    }
}

static void readerPagination(const Hegel & h, hegel_test_case_t * tc, const std::filesystem::path & directory)
{
    auto bytes = h.bytes(tc, 18000);
    // The explicit newlines exercise physical-line boundaries while arbitrary
    // bytes exercise reversible encoding and intra-line pagination.
    std::string original = "prefix\n" + bytes + "\nend";
    std::filesystem::path path;
    {
        Archive archive(directory);
        path = archive.path;
        archive.message(5, "fixture", original, [](uint64_t, uint64_t, std::string_view) {});
        archive.finish(0);
    }
    ReadOptions options;
    options.path = path;
    options.original = true;
    options.limit = static_cast<size_t>(h.integer(tc, 1, 10));
    std::string reconstructed;
    size_t pages = 0;
    for (;;) {
        std::string output, notice;
        readLog(options, [&](std::string_view s) { output += s; }, [&](std::string_view s) { notice += s; });
        require(output.size() <= 8192, "reader output must obey default byte budget");
        reconstructed += output;
        require(++pages < 2000, "reader pagination must make progress");
        auto continuation = notice.find("Continue:");
        if (continuation == std::string::npos) break;
        auto range = notice.find("--lines ", continuation);
        require(range != std::string::npos, "reader must supply an exact continuation range");
        range += 8;
        auto colon = notice.find(':', range);
        options.first = number(std::string_view(notice).substr(range, colon - range));
        auto offset = notice.find("--byte-offset ", continuation);
        options.offset = 0;
        if (offset != std::string::npos) {
            offset += 14;
            auto end = notice.find(' ', offset);
            options.offset = number(std::string_view(notice).substr(offset, end - offset));
        }
    }
    require(reconstructed == original, "original-format pagination must neither skip nor duplicate bytes");
    // A corrupt acceleration index must not prevent archive recovery.
    {
        std::ofstream index(path.string() + ".index", std::ios::trunc);
        index << "corrupt\n";
    }
    options.first = 1; options.offset = 0; options.all = true;
    std::string withoutIndex;
    readLog(options, [&](std::string_view s) { withoutIndex += s; }, [](std::string_view) {});
    require(withoutIndex == original, "archive must remain readable without a valid index");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".index");
}

static void retentionSafety(const Hegel & h, hegel_test_case_t * tc, const std::filesystem::path & directory)
{
    auto days = static_cast<uint64_t>(h.integer(tc, 0, 5));
    bool pressure = h.integer(tc, 0, 1) != 0;
    std::vector<uint64_t> ages;
    for (unsigned i = 0; i < 4; ++i) ages.push_back(static_cast<uint64_t>(h.integer(tc, 0, 8)));
    auto root = directory / "retention";
    std::filesystem::remove_all(root); // Owned by this property in the unique run directory.
    std::filesystem::create_directories(root);
    auto now = std::filesystem::file_time_type::clock::now();
    std::vector<std::filesystem::path> completed;
    for (size_t i = 0; i < ages.size(); ++i) {
        Archive archive(root);
        archive.message(i, "fixture", "evidence", [](uint64_t, uint64_t, std::string_view) {});
        archive.finish(0);
        completed.push_back(archive.path);
        std::filesystem::last_write_time(archive.path, now - std::chrono::hours(24 * ages[i] + 12));
    }
    std::filesystem::path incomplete;
    {
        Archive interrupted(root);
        incomplete = interrupted.path;
    }
    {
        Archive active(root);
        std::filesystem::last_write_time(incomplete, now - std::chrono::hours(24 * 100));
        std::filesystem::last_write_time(active.path, now - std::chrono::hours(24 * 100));
        auto link = root / "foreign.jsonl";
        std::filesystem::create_symlink(incomplete.filename(), link);
        pruneCompleted(root, {days, pressure ? 1ULL : 0ULL});
        require(std::filesystem::exists(active.path), "retention must not unlink an active writer");
        require(std::filesystem::exists(incomplete), "retention must preserve incomplete sessions");
        require(std::filesystem::is_symlink(link), "retention must not follow or delete symlink entries");
        for (size_t i = 0; i < ages.size(); ++i) {
            bool expired = days && ages[i] >= days;
            require(std::filesystem::exists(completed[i]) == !(expired || pressure),
                    "completed retention must implement age and aggregate-byte policies");
            require(std::filesystem::exists(completed[i].string() + ".index") == !(expired || pressure),
                    "retention must remove a pruned session's optional index");
        }
    }
    std::filesystem::remove_all(root);
}

static void edgeChecks(const std::filesystem::path & directory)
{
    {
        std::string console;
        Session session(directory / "confidence-priority", [&](std::string_view s) { console += s; });
        session.activity(4, "fixture");
        for (unsigned i = 0; i < 100; ++i)
            session.message(4, "possible failure " + std::to_string(i) + std::string(300, 'x'));
        session.message(4, R"({"severity":"error","message":"HIGH_CONFIDENCE_MARKER"})");
        session.finish(0);
        require(console.find("[rendered] HIGH_CONFIDENCE_MARKER") != std::string::npos,
            "heuristic noise must not exhaust the reserved high-confidence diagnostic budget");
    }
    {
        std::string console;
        Session session(directory / "context-window", [&](std::string_view s) { console += s; });
        session.activity(9, "bounded");
        for (unsigned i = 0; i < 50; ++i) session.message(9, "before " + std::to_string(i));
        session.message(9, "ERROR context trigger");
        for (unsigned i = 0; i < 50; ++i) session.message(9, "after " + std::to_string(i));
        require(console.find("L31 [bounded] before 30") != std::string::npos
            && console.find("L30 [bounded]") == std::string::npos
            && console.find("L71 [bounded] after 19") != std::string::npos
            && console.find("L72 [bounded]") == std::string::npos,
            "ordinary error context must include at most 20 preceding and 20 following lines");
        session.message(9, "ERROR timed window");
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        session.message(9, "LATE_CONTEXT_MARKER");
        session.finish(0);
        require(console.find("LATE_CONTEXT_MARKER") == std::string::npos,
            "following context must expire after its bounded time window");
    }
    {
        Archive archive(directory / "invalid-status");
        archive.append({{"kind", "completion"}, {"status", 4294967296ULL}});
        ReadOptions options;
        options.path = archive.path;
        bool refused = false;
        try { readLog(options, [](std::string_view) {}, [](std::string_view) {}); }
        catch (const std::runtime_error &) { refused = true; }
        require(refused, "untrusted oversized status must not narrow to a successful exit code");
    }
    {
        Archive archive(directory / "unicode-reader");
        std::string unicode;
        for (unsigned i = 0; i < 5000; ++i) unicode += "λ";
        archive.message(1, "fixture", unicode, [](uint64_t, uint64_t, std::string_view) {});
        archive.finish(0);
        ReadOptions options;
        options.path = archive.path;
        std::string output, notice;
        readLog(options, [&](std::string_view s) { output += s; }, [&](std::string_view s) { notice += s; });
        (void) Json(output).dump(); // Strict UTF-8 validation at a pagination boundary.
        require(output.size() <= 8192 && notice.find("--byte-offset") != std::string::npos,
            "rendered Unicode pagination must stay valid and supply byte continuation");
    }
    {
        std::string console;
        Session session(directory / "native-diagnostics", [&](std::string_view s) { console += s; });
        session.message(0, "error:\n trace A\n problem A", Severity::error, "primary A");
        session.message(0, "error:\n trace B\n problem B", Severity::error, "primary B");
        session.message(0, "error:\n trace A\n problem A", Severity::error, "primary A");
        session.finish(1);
        require(console.find("D2") != std::string::npos && console.find("[rendered] primary B") != std::string::npos
            && console.find("1 repeats") != std::string::npos,
            "native errors sharing an error header must deduplicate by whole event and show their primary message");
    }
    std::string output;
    auto root = directory / "edges";
    {
        Session session(root, [&](std::string_view s) { output += s; });
        session.activity(77, "failed-builder", "/nix/store/exact-fixture.drv");
        for (unsigned i = 0; i < 50; ++i) session.message(77, "setup " + std::to_string(i));
        session.endActivity(77);
        session.message(0, "error: builder for /nix/store/exact-fixture.drv exited with code 1", Severity::error);
        session.finish(1);
    }
    require(output.find("L11 [failed-builder] setup 10") != std::string::npos
        && output.find("L50 [failed-builder] setup 49") != std::string::npos,
        "actual native failure must recover 40 lines from its completed builder");
    require(output.find("L10 [failed-builder]") == std::string::npos, "failure fallback must cap its preceding context");

    // Resource limits apply only to this isolated child. A full archive device
    // must switch presentation to visible output without throwing into a build.
    auto child = ::fork();
    if (child < 0) systemFailure("forking archive failure fixture");
    if (child == 0) {
        std::string console;
        Session session(directory / "disk-limit", [&](std::string_view s) { console += s; });
        ::signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit{256, 256};
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(2);
        session.message(0, std::string(4096, 'x'));
        session.message(0, "VISIBLE_AFTER_CAPTURE_FAILURE");
        session.finish(0);
        ::_exit(console.find("capture failed") != std::string::npos
            && console.find("VISIBLE_AFTER_CAPTURE_FAILURE") != std::string::npos
            && console.find("SUCCESS") != std::string::npos ? 0 : 1);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) systemFailure("waiting for archive failure fixture");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "mid-write capture failure must preserve execution and restore output");
    std::cout << "edge fixtures: completed-builder context and mid-write capture fallback passed\n";
}

using Property = void (*)(const Hegel &, hegel_test_case_t *, const std::filesystem::path &);

struct ProcessResult
{
    int status;
    std::string out;
    std::string error;
};

static std::string readFile(const std::filesystem::path & path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

static std::string environment(const char * key)
{
    const char * value = std::getenv(key);
    if (!value) throw std::runtime_error(std::string("missing fixture environment: ") + key);
    return value;
}

static ProcessResult execute(const std::vector<std::string> & arguments, const std::filesystem::path & directory,
                             bool compact = true, const std::map<std::string, std::string> & overrides = {},
                             std::string_view capture = "", const std::filesystem::path & interruptReady = {}, bool tty = false)
{
    auto root = std::filesystem::absolute(directory / "native");
    std::filesystem::create_directories(root / "home");
    std::filesystem::create_directories(root / "etc");
    auto stdoutPath = root / (std::string(capture) + "stdout");
    auto stderrPath = root / (std::string(capture) + "stderr");
    Descriptor output(::open(stdoutPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
    Descriptor error(::open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
    require(output.fd >= 0 && error.fd >= 0, "process capture fixture must open");
    Descriptor terminalMaster, terminalSlave;
    if (tty && ::openpty(&terminalMaster.fd, &terminalSlave.fd, nullptr, nullptr, nullptr) < 0)
        systemFailure("creating terminal fixture");
    auto child = ::fork();
    if (child < 0) systemFailure("forking native test");
    if (child == 0) {
        ::dup2(output.fd, STDOUT_FILENO); ::dup2(tty ? terminalSlave.fd : error.fd, STDERR_FILENO);
        if (tty) {
            ::close(terminalMaster.fd);
            ::close(terminalSlave.fd);
            ::setenv("TERM", "xterm", 1);
            ::unsetenv("NO_COLOR"); ::unsetenv("NOCOLOR");
        }
        auto set = [&](const char * key, const std::string & value) { ::setenv(key, value.c_str(), 1); };
        set("NIX_STORE_DIR", (root / "store").string());
        set("NIX_STATE_DIR", (root / "state").string());
        set("NIX_LOCALSTATE_DIR", (root / "var").string());
        set("NIX_LOG_DIR", (root / "logs").string());
        set("NIX_CONF_DIR", (root / "etc").string());
        set("NIX_BUILD_LOG_DIR", (root / "sessions").string());
        set("NIX_USER_CONF_FILES", "/dev/null");
        set("HOME", (root / "home").string());
        set("NIX_REMOTE", "local");
        set("NIX_PATH", "");
        set("NIX_COMPACT_LOG", compact ? "1" : "0");
        set("NIX_CONFIG", "experimental-features = nix-command flakes\nsandbox = false\nbuild-users-group =\nsubstituters =\nflake-registry =\n");
        for (const auto & [key, value] : overrides) set(key.c_str(), value);
        std::vector<char *> argv;
        for (const auto & argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        ::execv(argv.front(), argv.data());
        ::_exit(127);
    }
    int status = 0;
    if (tty) {
        ::close(terminalSlave.fd); terminalSlave.fd = -1;
        std::string terminal;
        char buffer[1024];
        ssize_t count;
        while ((count = ::read(terminalMaster.fd, buffer, sizeof(buffer))) > 0)
            terminal.append(buffer, static_cast<size_t>(count));
        while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) systemFailure("waiting for terminal fixture");
        return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), readFile(stdoutPath), terminal};
    }
    if (!interruptReady.empty()) {
        bool ready = false;
        for (unsigned attempt = 0; attempt < 500; ++attempt) {
            if (std::filesystem::exists(interruptReady)) { ready = true; break; }
            if (::waitpid(child, &status, WNOHANG) == child)
                throw std::runtime_error("interrupt fixture exited before readiness: " + readFile(stderrPath));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::kill(child, SIGINT);
        while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) systemFailure("waiting for interrupted native test");
        require(ready, "interrupt fixture must publish readiness");
        return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), readFile(stdoutPath), readFile(stderrPath)};
    }
    while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) systemFailure("waiting for native test");
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), readFile(stdoutPath), readFile(stderrPath)};
}

class DaemonFixture
{
    pid_t child = -1;
public:
    std::filesystem::path socket;
    DaemonFixture(const std::string & binary, const std::filesystem::path & directory)
    {
        socket = std::filesystem::absolute(directory / "daemon.socket");
        child = ::fork();
        if (child < 0) systemFailure("forking daemon fixture");
        if (child == 0) {
            ::setpgid(0, 0);
            try {
                auto result = execute({binary, "daemon"}, directory, false,
                    {{"NIX_DAEMON_SOCKET_PATH", socket.string()}}, "daemon-");
                ::_exit(result.status);
            } catch (...) { ::_exit(2); }
        }
        ::setpgid(child, child);
    }
    void ready() const
    {
        for (unsigned attempt = 0; attempt < 500; ++attempt) {
            if (std::filesystem::exists(socket)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("daemon fixture did not create its socket");
    }
    ~DaemonFixture()
    {
        if (child > 0) {
            ::kill(-child, SIGTERM);
            int status = 0;
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
    }
};

static std::string expression(std::string_view script)
{
    static uint64_t sequence = 0;
    return "builtins.derivation { name = \"compact-fixture-" + std::to_string(++sequence)
        + "\"; system = " + Json(environment("NIX_COMPACT_TEST_SYSTEM")).dump()
        + "; builder = " + Json(environment("NIX_COMPACT_TEST_BASH")).dump()
        + "; args = [ \"-c\" " + Json(script).dump() + " ]; }";
}

static void nativeBuildResults(const Hegel & h, hegel_test_case_t * tc, const std::filesystem::path & directory)
{
    auto exit = h.integer(tc, 0, 3);
    auto padding = h.bytes(tc, 300);
    std::string marker = "ERROR fatal failure " + hex(padding);
    auto source = expression("printf '%s\\n' '" + marker + "'; printf done > \"$out\"; exit " + std::to_string(exit));
    auto binary = environment("NIX_COMPACT_TEST_BINARY");
    auto raw = execute({binary, "build", "--no-link", "--expr", source, "--print-build-logs"}, directory, false);
    std::vector<std::string> command{binary, "build", "--no-link", "--expr", source};
    if (exit == 0) command.push_back("--rebuild"); // Rebuild requires an existing successful output.
    auto filtered = execute(command, directory);
    require(filtered.status == raw.status, "native compact build must preserve actual upstream exit status");
    require((filtered.status == 0) == (exit == 0), "fixture must actually exercise builder success and failure");
    auto first = filtered.error.find("Full log: ");
    if (first == std::string::npos) {
        std::cerr << filtered.error;
        require(false, "native build must advertise an archive");
    }
    first += 10;
    auto last = filtered.error.find('\n', first);
    auto log = records(filtered.error.substr(first, last - first));
    bool found = false;
    for (const auto & record : log) {
        if (text(record, "kind") == "line" && payload(record).find(marker) != std::string::npos) found = true;
    }
    require(found, "real builder output must reach the native archive");
    require(log.back().at("status") == filtered.status, "native completion footer must match the process result");
    auto next = execute({binary, "build", "--no-link", "--expr", expression("printf done > \"$out\"")}, directory);
    require(next.status == 0, "error-like text and failed builds must not poison the next invocation");
}

static std::filesystem::path advertisedLog(const ProcessResult & result)
{
    auto start = result.error.find("Full log: ");
    require(start != std::string::npos, "runtime check requires an advertised session");
    start += 10;
    return result.error.substr(start, result.error.find('\n', start) - start);
}

static void runtimeChecks(const std::filesystem::path & directory)
{
    auto binary = environment("NIX_COMPACT_TEST_BINARY");
    auto source = expression("for ((i=0;i<10000;i++)); do printf 'ordinary line %s\\n' \"$i\"; done; printf done > \"$out\"");
    auto compact = execute({binary, "build", "--no-link", "--json", "--expr", source}, directory);
    require(compact.status == 0 && Json::parse(compact.out).is_array(), "JSON stdout must remain machine-readable");
    auto logPath = advertisedLog(compact);
    auto archive = records(logPath);
    size_t ordinary = 0;
    for (const auto & record : archive)
        if (text(record, "kind") == "line" && payload(record).starts_with("ordinary line ")) ++ordinary;
    require(ordinary == 10000, "noisy success must archive every builder line");
    auto raw = execute({binary, "build", "--no-link", "--rebuild", "--print-build-logs", "--expr", source}, directory, false);
    require(raw.status == 0, "raw comparison build must succeed");
    require(compact.error.size() * 20 < raw.error.size(), "compact noisy success must reduce console bytes by at least 95 percent");
    std::cout << "noisy success: compact=" << compact.error.size() << " bytes; raw=" << raw.error.size() << " bytes; archived=" << ordinary << " builder lines\n";

    auto reader = execute({binary, "build-log", logPath.string(), "--lines", "257:260"}, directory);
    require(reader.status == 0 && reader.out.find("L257 ") != std::string::npos && reader.out.find("L260 ") != std::string::npos,
            "native reader must resolve stable indexed line ranges");
    auto paths = execute({binary, "build", "--no-link", "--print-out-paths", "--expr", source}, directory);
    require(paths.status == 0 && paths.out.starts_with('/') && paths.out.find("[nix]") == std::string::npos,
            "output paths must remain clean stdout");
    auto terminal = execute({binary, "build", "--no-link", "--rebuild", "--expr", source}, directory, true, {}, "", {}, true);
    auto noticeAt = terminal.error.find("[nix] Output suppressed");
    require(terminal.status == 0 && noticeAt != std::string::npos
        && terminal.error.find('\033', noticeAt) == std::string::npos,
        "TTY compact mode must not redraw or emit ANSI escapes after the suppression notice");
    auto internal = execute({binary, "build", "--no-link", "--rebuild", "--log-format", "internal-json", "--expr", source}, directory);
    require(internal.status == 0 && internal.error.find("Output suppressed") == std::string::npos
        && internal.error.find("@nix ") != std::string::npos, "explicit internal-json must preserve upstream protocol");
    auto full = execute({binary, "build", "--no-link", "--rebuild", "-L", "--expr", source}, directory);
    require(full.status == 0 && full.error.find("ordinary line 9999") != std::string::npos
        && full.error.find("Output suppressed") == std::string::npos, "explicit full streaming must bypass suppression");
    auto fallback = execute({binary, "build", "--no-link", "--rebuild", "--expr", source}, directory, true,
        {{"NIX_BUILD_LOG_DIR", "/dev/null/not-a-directory"}});
    require(fallback.status == 0 && fallback.error.find("capture failed") != std::string::npos
        && fallback.error.find("ordinary line 9999") != std::string::npos, "capture failure must visibly restore build output");

    auto evaluation = execute({binary, "build", "--no-link", "--expr", "throw \"evaluation fixture\""}, directory);
    auto evalArchive = records(advertisedLog(evaluation));
    size_t diagnostics = 0;
    for (const auto & record : evalArchive)
        if (text(record, "kind") == "diagnostic" && text(record, "severity") == "error") ++diagnostics;
    if (evaluation.status == 0 || diagnostics != 1) std::cerr << evaluation.error;
    require(evaluation.status != 0 && diagnostics == 1, "multiline native errors must be one diagnostic event");

    // Both SSH stores bypass transport for localhost but execute the actual
    // daemon/serve protocols in child processes, independently of local capture.
    auto bin = std::filesystem::path(binary).parent_path();
    for (const std::string protocol : {"ssh-ng", "ssh"}) {
        auto program = (bin / (protocol == "ssh-ng" ? "nix-daemon" : "nix-store")).string();
        auto remoteRoot = std::filesystem::absolute(directory / ("remote-" + protocol));
        auto url = protocol + "://localhost?remote-program=" + program + "&remote-store=" + remoteRoot.string();
        auto remoteExpression = "builtins.derivation { name = \"remote-fixture-" + protocol + "\"; system = "
            + Json(environment("NIX_COMPACT_TEST_SYSTEM")).dump() + "; builder = builtins.path { path = "
            + Json(environment("NIX_COMPACT_TEST_BUSYBOX")).dump() + "; name = \"busybox\"; }; args = [ \"sh\" \"-c\" "
            + Json("printf 'REMOTE_CAPTURE_MARKER\\n'; printf done > \"$out\"").dump() + " ]; }";
        auto remote = execute({binary, "build", "--impure", "--store", std::filesystem::absolute(directory / "local-remote-root").string(),
            "--max-jobs", "0", "--builders",
            url + " " + environment("NIX_COMPACT_TEST_SYSTEM") + " - 1 1", "--no-link", "--expr",
            remoteExpression}, directory, true, {{"NIX_STORE_DIR", "/nix/store"}});
        if (remote.status != 0) std::cerr << remote.error;
        require(remote.status == 0, "remote-protocol fixture must build successfully");
        bool found = false;
        for (const auto & record : records(advertisedLog(remote)))
            if (text(record, "kind") == "line" && payload(record).find("REMOTE_CAPTURE_MARKER") != std::string::npos) found = true;
        require(found, "remote-protocol builder output must reach archive");
    }

    auto quiet = execute({binary, "build", "--no-link", "--expr", expression(
        environment("NIX_COMPACT_TEST_SLEEP") + " 16; printf done > \"$out\"")}, directory);
    uint64_t lastHeartbeat = 0;
    std::istringstream quietLines(quiet.error);
    std::string quietLine;
    while (std::getline(quietLines, quietLine)) {
        if (!quietLine.starts_with("[nix] ")) continue;
        auto suffix = quietLine.find("s: ", 6);
        if (suffix == std::string::npos || suffix == 6
            || !std::all_of(quietLine.begin() + 6, quietLine.begin() + suffix,
                [](unsigned char c) { return std::isdigit(c); })) continue;
        auto seconds = number(std::string_view(quietLine).substr(6, suffix - 6));
        require(seconds >= lastHeartbeat + 15, "progress updates must not exceed the 15-second rate");
        lastHeartbeat = seconds;
    }
    require(quiet.status == 0 && lastHeartbeat >= 15,
            "quiet real builds must emit a heartbeat even when scheduling delays its timer");
    {
        DaemonFixture daemon(binary, directory);
        daemon.ready();
        auto result = execute({binary, "build", "--store", "unix://" + daemon.socket.string(), "--no-link", "--expr",
            expression("printf 'DAEMON_CAPTURE_MARKER\\n'; printf done > \"$out\"")}, directory);
        if (result.status != 0) std::cerr << result.error;
        require(result.status == 0, "real Unix-socket daemon build must succeed");
        bool found = false;
        for (const auto & record : records(advertisedLog(result)))
            if (text(record, "kind") == "line" && payload(record).find("DAEMON_CAPTURE_MARKER") != std::string::npos) found = true;
        require(found, "real Unix-socket daemon must forward builder logs into compact capture");
    }
    auto readyFile = std::filesystem::absolute(directory / "interrupt-ready");
    auto interruptedSource = expression("printf ready > " + quote(readyFile.string()) + "; "
        + environment("NIX_COMPACT_TEST_SLEEP") + " 60; printf done > \"$out\"");
    auto interrupted = execute({binary, "build", "--no-link", "--expr", interruptedSource}, directory,
        true, {}, "", readyFile);
    auto interruptedArchive = records(advertisedLog(interrupted));
    require(interrupted.status != 0 && interruptedArchive.back().at("status") == interrupted.status,
            "interrupted build must retain actual nonzero exit status in the completion footer");
    std::filesystem::remove(readyFile);
    auto rawInterrupted = execute({binary, "build", "--no-link", "--expr", interruptedSource}, directory,
        false, {}, "", readyFile);
    require(rawInterrupted.status == interrupted.status, "compact capture must preserve upstream SIGINT exit semantics");
    std::cout << "native runtime: JSON, paths, TTY, indexed reader, full streaming, capture fallback, evaluation, Unix daemon, remote protocols, heartbeat and SIGINT passed\n";
}

static bool run(const char * name, Property property, const std::filesystem::path & directory, uint64_t examples = 200)
{
    const char * selected = std::getenv("NIX_COMPACT_PROPERTY");
    if (selected && std::string_view(selected) != name) return true;
    Hegel h;
    hegel_settings_t * settings = nullptr;
    h.check(hegel_settings_new(h.context, &settings));
    h.check(hegel_settings_set_test_cases(h.context, settings, examples));
    h.check(hegel_settings_set_database(h.context, settings, ""));
    h.check(hegel_settings_set_derandomize(h.context, settings, true));
    if (const char * replay = std::getenv("NIX_COMPACT_REPLAY")) {
        require(selected != nullptr, "replay requires NIX_COMPACT_PROPERTY to name the property");
        hegel_test_case_t * tc = nullptr;
        h.check(hegel_test_case_from_blob(h.context, settings, replay, nullptr, nullptr, &tc));
        bool passed = false;
        try {
            property(h, tc, directory);
            passed = true;
            h.check(hegel_mark_complete(h.context, tc, HEGEL_STATUS_VALID, nullptr));
        } catch (const DrawStopped &) {
            std::cerr << name << ": replay no longer matches this generator\n";
            h.check(hegel_mark_complete(h.context, tc, HEGEL_STATUS_OVERRUN, nullptr));
        } catch (const std::exception & error) {
            std::cerr << name << ": reproduced: " << error.what() << '\n';
            h.check(hegel_mark_complete(h.context, tc, HEGEL_STATUS_INTERESTING, error.what()));
        }
        h.check(hegel_test_case_free(h.context, tc));
        h.check(hegel_settings_free(h.context, settings));
        std::cout << name << ": replay " << (passed ? "passed" : "failed") << '\n';
        return passed;
    }
    hegel_run_t * run = nullptr;
    h.check(hegel_run_start(h.context, settings, nullptr, nullptr, &run));
    size_t cases = 0;
    while (true) {
        hegel_test_case_t * tc = nullptr;
        h.check(hegel_next_test_case(h.context, run, &tc));
        if (!tc) break;
        try {
            property(h, tc, directory);
            h.check(hegel_mark_complete(h.context, tc, HEGEL_STATUS_VALID, nullptr));
            ++cases;
        } catch (const DrawStopped &) {
            h.check(hegel_mark_complete(h.context, tc, HEGEL_STATUS_OVERRUN, nullptr));
        } catch (const std::exception & error) {
            h.check(hegel_mark_complete(h.context, tc, HEGEL_STATUS_INTERESTING, error.what()));
            // Remove failed fixture state so Hegel shrinking starts cleanly.
            for (const auto & entry : std::filesystem::directory_iterator(directory))
                if (entry.path().extension() == ".jsonl") std::filesystem::remove(entry.path());
        }
        h.check(hegel_test_case_free(h.context, tc));
    }
    hegel_run_result_t * result = nullptr;
    h.check(hegel_run_result(h.context, run, &result));
    hegel_run_status_t status;
    h.check(hegel_run_result_status(h.context, result, &status));
    size_t failures = 0;
    h.check(hegel_run_result_failure_count(h.context, result, &failures));
    for (size_t i = 0; i < failures; ++i) {
        hegel_failure_t * failure = nullptr;
        h.check(hegel_run_result_failure(h.context, result, i, &failure));
        const char * origin = nullptr;
        const char * blob = nullptr;
        h.check(hegel_failure_origin(h.context, failure, &origin));
        h.check(hegel_failure_reproduction_blob(h.context, failure, &blob));
        std::cerr << name << ": " << origin << "\nHegel reproduction: " << blob << '\n';
        h.check(hegel_failure_free(h.context, failure));
    }
    std::cout << name << ": " << cases << " valid cases; " << failures << " failures\n";
    h.check(hegel_run_result_free(h.context, result));
    h.check(hegel_run_free(h.context, run));
    h.check(hegel_settings_free(h.context, settings));
    return status == HEGEL_RUN_STATUS_PASSED;
}

int main(int argc, char ** argv) try
{
    if (argc != 2) return 2;
    if (const char * selected = std::getenv("NIX_COMPACT_PROPERTY")) {
        const std::set<std::string> known{"archive-round-trip", "structured-precedence", "budgets-interleaving-and-result",
            "reader-lossless-pagination", "retention-preserves-active-and-incomplete", "native-build-result-and-capture"};
        require(known.contains(selected), "unknown property selection");
        if (std::string_view(selected) == "native-build-result-and-capture")
            require(std::getenv("NIX_COMPACT_TEST_BINARY") != nullptr, "native property requires a configured test binary");
    }
    std::filesystem::path parent(argv[1]);
    std::filesystem::create_directories(parent);
    auto directory = parent / ("run-" + std::to_string(::getpid()) + "-" + timestamp());
    require(std::filesystem::create_directory(directory), "test fixtures require a newly owned directory");
    bool passed = run("archive-round-trip", roundTrip, directory);
    passed = run("structured-precedence", structuredPrecedence, directory) && passed;
    passed = run("budgets-interleaving-and-result", budgetsAndInterleaving, directory) && passed;
    passed = run("reader-lossless-pagination", readerPagination, directory) && passed;
    passed = run("retention-preserves-active-and-incomplete", retentionSafety, directory) && passed;
    if (!std::getenv("NIX_COMPACT_PROPERTY")) edgeChecks(directory);
    if (std::getenv("NIX_COMPACT_TEST_BINARY"))
        passed = run("native-build-result-and-capture", nativeBuildResults, directory, 50) && passed;
    if (std::getenv("NIX_COMPACT_TEST_BINARY") && !std::getenv("NIX_COMPACT_PROPERTY")) runtimeChecks(directory);
    if (passed) std::filesystem::remove_all(directory);
    return passed ? 0 : 1;
}
catch (const std::exception & error)
{
    std::cerr << "native/property fixture failed: " << error.what() << '\n';
    return 1;
}
