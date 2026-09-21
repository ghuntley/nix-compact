#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace nix::compact {

enum class Severity { routine, warning, error };
enum class Confidence { heuristic, text, structured, native };

struct Diagnostic
{
    Severity severity = Severity::routine;
    Confidence confidence = Confidence::heuristic;
    std::string source = "text";
    std::string rendered;
};

inline std::string lower(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

inline std::optional<Severity> severity(std::string_view value)
{
    auto s = lower(value);
    if (s == "error" || s == "fatal" || s == "critical" || s == "alert" || s == "emergency")
        return Severity::error;
    if (s == "warn" || s == "warning")
        return Severity::warning;
    if (s == "trace" || s == "debug" || s == "info" || s == "notice")
        return Severity::routine;
    return {};
}

// Linear-time terminal normalization: no regex engine or interpretation of
// cursor movement. The archive keeps the unmodified input separately.
inline std::string plain(std::string_view input)
{
    std::string output;
    enum class Escape { none, start, csi, osc, oscEnd };
    Escape escape = Escape::none;
    for (unsigned char c : input) {
        if (escape == Escape::start) {
            escape = c == '[' ? Escape::csi : c == ']' ? Escape::osc : Escape::none;
        } else if (escape == Escape::csi) {
            if (c >= 0x40 && c <= 0x7e) escape = Escape::none;
        } else if (escape == Escape::osc) {
            if (c == 7) escape = Escape::none;
            else if (c == 27) escape = Escape::oscEnd;
        } else if (escape == Escape::oscEnd) {
            escape = c == '\\' ? Escape::none : Escape::osc;
        } else if (c == 27) {
            escape = Escape::start;
        } else if (c == '\t' || c == '\n' || c >= 0x20) {
            if (c != 0x7f) output += static_cast<char>(c);
        }
    }
    // Console text and derived metadata must remain valid UTF-8 even when a
    // builder writes arbitrary bytes. The original bytes are archived separately.
    auto encoded = nlohmann::json(output).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return nlohmann::json::parse(encoded).get<std::string>();
}

inline const nlohmann::json * member(const nlohmann::json & object, std::string_view key)
{
    if (!object.is_object()) return nullptr;
    auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

inline std::string text(const nlohmann::json & object, std::string_view key)
{
    auto value = member(object, key);
    return value && value->is_string() ? value->get<std::string>() : std::string{};
}

// Bound nesting before invoking the JSON parser. Braces inside strings do not
// count; malformed strings are rejected by the parser itself.
inline bool shallow(std::string_view input)
{
    size_t depth = 0;
    bool quoted = false, escaped = false;
    for (char c : input) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 32) return false;
        } else if (c == '}' || c == ']') {
            if (!depth) return false;
            --depth;
        }
    }
    return true;
}

inline bool word(std::string_view input, std::string_view token)
{
    auto isWord = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    size_t from = 0;
    while (true) {
        size_t at = input.find(token, from);
        if (at == std::string_view::npos) return false;
        size_t end = at + token.size();
        if ((at == 0 || !isWord(input[at - 1])) && (end == input.size() || !isWord(input[end])))
            return true;
        from = end;
    }
}

inline Diagnostic classify(std::string_view payload)
{
    // Parsing work is capped independently of archival storage.
    auto cleaned = plain(payload.substr(0, 65536));
    std::string_view input(cleaned);
    auto first = input.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos) input.remove_prefix(first);

    if (payload.size() <= 65536 && input.starts_with('{') && shallow(input)) {
        auto object = nlohmann::json::parse(input, nullptr, false);
        if (object.is_object()) {
            std::string source;
            std::string level;
            std::string rendered;
            auto reason = text(object, "reason");
            if (reason == "compiler-message") {
                source = "cargo";
                if (auto message = member(object, "message")) {
                    level = text(*message, "level");
                    rendered = text(*message, "rendered");
                }
            } else if (reason == "compiler-artifact" || reason == "build-script-executed") {
                return {Severity::routine, Confidence::structured, "cargo", {}};
            } else if (reason == "build-finished") {
                auto success = member(object, "success");
                if (success && success->is_boolean())
                    return {success->get<bool>() ? Severity::routine : Severity::error,
                            Confidence::structured, "cargo", {}};
            } else if (member(object, "severity")) {
                source = "severity-json";
                level = text(object, "severity");
                if (auto message = member(object, "message"))
                    rendered = message->is_string() ? message->get<std::string>() : message->dump(2);
            } else if (member(object, "level")) {
                source = "level-json";
                level = text(object, "level");
                rendered = text(object, "message");
                if (auto fields = member(object, "fields")) {
                    if (rendered.empty()) rendered = text(*fields, "message");
                    auto error = text(*fields, "error");
                    if (!error.empty()) rendered += "\nerror: " + error;
                }
            }
            if (auto parsed = severity(level))
                return {*parsed, Confidence::structured, source, rendered};
        }
    }

    auto s = lower(input);
    if (s.find("[error]") != std::string::npos || s.starts_with("error:")
        || s.starts_with("error[e") || s.starts_with("** (")
        || s.starts_with("== compilation error")
        || (s.starts_with("thread ") && s.find(" panicked at ") != std::string::npos))
        return {Severity::error, Confidence::text, "text", {}};
    if (s.find("[warning]") != std::string::npos || s.find("[warn]") != std::string::npos
        || s.starts_with("warning:"))
        return {Severity::warning, Confidence::text, "text", {}};

    // Remove exact zero-count phrases from heuristic consideration rather than
    // suppressing the whole line: "0 failures; fatal shutdown" is still useful.
    for (std::string_view phrase : {"0 failures", "0 errors", "0 failed", "0 warnings"}) {
        size_t at = 0;
        while ((at = s.find(phrase, at)) != std::string::npos) {
            if (at == 0 || !std::isdigit(static_cast<unsigned char>(s[at - 1])))
                s.replace(at, phrase.size(), phrase.size(), ' ');
            at += phrase.size();
        }
    }
    for (auto token : {"error", "fatal", "panic", "panicked", "failed", "failure", "failures", "exception"})
        if (word(s, token)) return {Severity::error, Confidence::heuristic, "text-candidate", {}};
    for (auto token : {"warn", "warning"})
        if (word(s, token)) return {Severity::warning, Confidence::heuristic, "text-candidate", {}};
    return {};
}

} // namespace nix::compact
