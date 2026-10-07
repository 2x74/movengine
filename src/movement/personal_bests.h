#pragma once

#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace movement {

// best times per map, track, style and movement settings, saved between sessions. a time only counts against runs with the exact same settings (sv_* and tickrate as one string, see settingsKey in the app) so changing a setting hides your pb till you change it back
class PersonalBests {
public:
    struct Entry {
        int track = 0;
        std::string style;
        std::string settings;  // "" = from before settings were recorded
        float seconds = 0.0f;
    };

    // -1 when there's no time yet.
    float get(int track, const std::string& style, const std::string& settings) const;
    // records a finished run, true if it's a new pb for these settings (first finish counts)
    bool submit(int track, const std::string& style, const std::string& settings, float seconds);
    // every time on a track, fastest first
    std::vector<Entry> list(int track) const;

    // "<track> <seconds> <style name> | <settings>" per line; a missing file
    // is just no times yet.
    void load(const std::string& path);
    bool save(const std::string& path) const;

private:
    std::map<std::tuple<int, std::string, std::string>, float> times_;
};

}  // namespace movement
