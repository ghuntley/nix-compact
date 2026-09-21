#include "archive.hh"

#include <iostream>
#include <vector>

using namespace nix::compact;

int main(int argc, char ** argv)
{
    if (argc != 2) return 2;
    std::filesystem::path directory(argv[1]);
    std::filesystem::path path;
    std::vector<uint64_t> lines;
    const std::string original = std::string("first\n\ninvalid ") + char(0xff) + "\nlast";
    {
        Archive archive(directory);
        path = archive.path;
        archive.message(42, "fixture", original,
            [&](uint64_t line, uint64_t, std::string_view) { lines.push_back(line); });
        if (lines != std::vector<uint64_t>{1, 2, 3, 4}) return 1;
        archive.append({{"kind", "diagnostic"}, {"id", 1}, {"line", 3}});
        archive.finish(0);
        archive.finish(1); // Completion is idempotent, never rewritten.
    }
    std::ifstream file(path);
    std::string line, restored;
    unsigned completions = 0;
    while (std::getline(file, line)) {
        auto record = Json::parse(line);
        if (record.at("kind") == "line") {
            std::string payload = record.at("payload").get<std::string>();
            restored += record.at("encoding") == "hex" ? unhex(payload) : payload;
            if (record.at("newline").get<bool>()) restored += '\n';
        } else if (record.at("kind") == "completion") {
            if (record.at("status") != 0) return 1;
            ++completions;
        }
    }
    if (restored != original || completions != 1) return 1;
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0 || (info.st_mode & 0777) != 0600) return 1;
    if (::stat(directory.c_str(), &info) != 0 || (info.st_mode & 0777) != 0700) return 1;
    {
        Archive incomplete(directory);
        if (incomplete.path == path) return 1;
        path = incomplete.path;
    }
    std::ifstream incomplete(path);
    while (std::getline(incomplete, line))
        if (Json::parse(line).at("kind") == "completion") return 1;
    if (quote("a'b") != "'a'\\''b'") return 1;
    std::cout << "archive byte round-trip, numbering, permissions, unique sessions and completion passed\n";
}
