#include "console/cfg_exec.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace console {

namespace {

std::vector<std::string> tokenizeLine(const std::string& line) {
    std::vector<std::string> tokens;
    size_t i = 0;
    size_t n = line.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        if (line[i] == '/' && i + 1 < n && line[i + 1] == '/') {
            break;  // rest of the line is a comment
        }
        if (line[i] == '"') {
            size_t end = line.find('"', i + 1);
            if (end == std::string::npos) {
                end = n;
            }
            tokens.push_back(line.substr(i + 1, end - i - 1));
            i = end + 1;
        } else {
            size_t start = i;
            while (i < n && !std::isspace(static_cast<unsigned char>(line[i]))) {
                ++i;
            }
            tokens.push_back(line.substr(start, i - start));
        }
    }
    return tokens;
}

void execConfigFileImpl(const std::string& path, CvarRegistry& cvars, int depth) {
    if (depth > 8) {
        std::fprintf(stderr, "cfg exec recursion too deep, aborting at: %s\n", path.c_str());
        return;
    }

    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "cfg: could not open %s\n", path.c_str());
        return;
    }
    std::printf("cfg: executing %s\n", path.c_str());

    std::filesystem::path dir = std::filesystem::path(path).parent_path();

    std::string line;
    while (std::getline(file, line)) {
        auto tokens = tokenizeLine(line);
        if (tokens.empty()) {
            continue;
        }
        const std::string& cmd = tokens[0];

        if (cmd == "exec" && tokens.size() >= 2) {
            std::filesystem::path relative = dir / tokens[1];
            if (std::filesystem::exists(relative)) {
                execConfigFileImpl(relative.string(), cvars, depth + 1);
            } else {
                execConfigFileImpl(tokens[1], cvars, depth + 1);
            }
            continue;
        }

        if (tokens.size() >= 2 && cvars.has(cmd)) {
            try {
                float value = std::stof(tokens[1]);
                cvars.set(cmd, value);
                std::printf("cfg: %s %.3f\n", cmd.c_str(), value);
            } catch (const std::exception&) {
                std::fprintf(stderr, "cfg: couldn't parse value for %s: %s\n", cmd.c_str(), tokens[1].c_str());
            }
            continue;
        }

        std::printf("cfg: skipping unrecognized line: %s\n", line.c_str());
    }
}

}  // namespace

void execConfigFile(const std::string& path, CvarRegistry& cvars) {
    execConfigFileImpl(path, cvars, 0);
}

const char* const kSettingsHeader = "// movengine settings: only values changed from their defaults";

bool saveConfigFile(const std::string& path, const CvarRegistry& cvars) {
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "cfg: failed to write %s\n", path.c_str());
        return false;
    }
    file << kSettingsHeader << '\n';
    char value[32];
    for (const auto& cvar : cvars.all()) {
        if (cvar.value == cvar.defaultValue) continue;
        std::snprintf(value, sizeof(value), "%.6g", cvar.value);
        file << cvar.name << ' ' << value << '\n';
    }
    return true;
}

}  // namespace console
