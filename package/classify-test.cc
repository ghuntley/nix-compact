#include "classify.hh"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace nix::compact;

struct Case
{
    std::string input;
    Severity expected;
    Confidence confidence;
};

int main()
{
    const std::vector<Case> cases{
        {R"({"severity":"critical","message":{"reason":"timeout"}})", Severity::error, Confidence::structured},
        {R"({"severity":"info","message":"testing error handling","metadata":{"severity":"error"}})", Severity::routine, Confidence::structured},
        {R"({"level":"ERROR","fields":{"message":"connection refused"},"spans":[{"name":"request"}]})", Severity::error, Confidence::structured},
        {R"({"level":"WARN","message":"retrying"})", Severity::warning, Confidence::structured},
        {R"({"level":"INFO","fields":{"error":"expected"}})", Severity::routine, Confidence::structured},
        {R"({"reason":"compiler-message","message":{"level":"error","rendered":"error[E0425]: unknown value\n --> src/main.rs:42"}})", Severity::error, Confidence::structured},
        {R"({"reason":"compiler-artifact","target":{"name":"error-utils"}})", Severity::routine, Confidence::structured},
        {R"({"reason":"build-finished","success":false})", Severity::error, Confidence::structured},
        {R"({"reason":"build-finished","success":"false"})", Severity::routine, Confidence::heuristic},
        {"12:34:56 [error] disconnected", Severity::error, Confidence::text},
        {"\033[31merror[E0308]: mismatch\033[0m", Severity::error, Confidence::text},
        {"** (RuntimeError) connection refused", Severity::error, Confidence::text},
        {"thread 'main' panicked at src/main.rs:42", Severity::error, Confidence::text},
        {"24 tests, 0 failures", Severity::routine, Confidence::heuristic},
        {"24 tests, 10 failures", Severity::error, Confidence::heuristic},
        {"0 failures; fatal shutdown", Severity::error, Confidence::heuristic},
        {"compiling error_handler", Severity::routine, Confidence::heuristic},
        {"database connection failed", Severity::error, Confidence::heuristic},
        {"{malformed: error", Severity::error, Confidence::heuristic},
        {R"({"level":42,"message":"error"})", Severity::error, Confidence::heuristic},
    };
    size_t index = 0;
    for (const auto & test : cases) {
        auto actual = classify(test.input);
        if (actual.severity != test.expected || actual.confidence != test.confidence) {
            std::cerr << "classification fixture " << index << " failed\n";
            return EXIT_FAILURE;
        }
        ++index;
    }
    if (plain("a\033]0;error title\007b") != "ab") return EXIT_FAILURE;
    if (shallow(std::string(33, '['))) return EXIT_FAILURE;
    if (!shallow(R"({"message":"{{{{[[[["})")) return EXIT_FAILURE;
    auto cargo = classify(cases[5].input);
    if (cargo.rendered.find('\n') == std::string::npos) return EXIT_FAILURE;
    // Invalid UTF-8 must degrade to text without JSON throwing.
    const std::string invalid = std::string("{\"level\":\"ERROR\",\"message\":\"") + char(0xff) + "\"}";
    if (classify(invalid).severity != Severity::error) return EXIT_FAILURE;
    std::cout << cases.size() << " classification fixtures and parser bounds passed\n";
}
