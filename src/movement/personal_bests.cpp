#include "movement/personal_bests.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace movement {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(' ');
    size_t b = s.find_last_not_of(' ');
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

float PersonalBests::get(int track, const std::string& style, const std::string& settings) const {
    auto it = times_.find({track, style, settings});
    return it == times_.end() ? -1.0f : it->second;
}

bool PersonalBests::submit(int track, const std::string& style, const std::string& settings, float seconds) {
    float& best = times_.try_emplace({track, style, settings}, -1.0f).first->second;
    if (best >= 0.0f && seconds >= best) return false;
    best = seconds;
    return true;
}

std::vector<PersonalBests::Entry> PersonalBests::list(int track) const {
    std::vector<Entry> out;
    for (const auto& [key, seconds] : times_) {
        if (std::get<0>(key) == track) out.push_back({track, std::get<1>(key), std::get<2>(key), seconds});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.seconds < b.seconds; });
    return out;
}

void PersonalBests::load(const std::string& path) {
    times_.clear();
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        int track = 0;
        float seconds = 0.0f;
        std::string rest;
        if (!(iss >> track >> seconds)) continue;
        std::getline(iss >> std::ws, rest);
        size_t bar = rest.find('|');
        std::string style = trim(rest.substr(0, bar));
        std::string settings = bar == std::string::npos ? "" : trim(rest.substr(bar + 1));
        if (style.empty() || seconds <= 0.0f) continue;
        times_[{track, style, settings}] = seconds;
    }
}

bool PersonalBests::save(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "failed to write times to %s\n", path.c_str());
        return false;
    }
    char buf[32];
    for (const auto& [key, seconds] : times_) {
        std::snprintf(buf, sizeof(buf), "%.6f", seconds);
        file << std::get<0>(key) << ' ' << buf << ' ' << std::get<1>(key) << " | " << std::get<2>(key) << '\n';
    }
    return true;
}

}  // namespace movement
