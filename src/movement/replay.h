#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace movement {

// one physics tick of a recorded run. position is the aabb centre, same as PlayerState so playback doesn't have to guess hull height
struct ReplayFrame {
    glm::vec3 position{0.0f};
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
    bool ducking = false;
};

// a finished run, recorded at the fixed tick rate. style and settings are kept so a replay from different movement settings can be spotted instead of silently played back like it's comparable
struct Replay {
    float seconds = 0.0f;         // the finishing time this run was worth
    float tickInterval = 0.01f;   // seconds per frame, as recorded
    int track = 0;
    std::string style;
    std::string settings;
    std::vector<ReplayFrame> frames;

    bool empty() const { return frames.empty(); }
    // playback length, which is the frame count rather than `seconds`: the timer starts and stops on zone crossings part way through a tick so the two differ by up to a tick
    float duration() const { return static_cast<float>(frames.size()) * tickInterval; }
    // position/angles at any time, interpolated between ticks so playback is smooth at any frame rate instead of stepping at 100t
    ReplayFrame sample(float seconds) const;
};

// compact binary file, runs are thousands of frames and a text format would be bigger and lossy about float round tripping
bool saveReplay(const std::string& path, const Replay& replay);
// false (leaving `out` untouched) when the file is missing, truncated, or not a replay this build understands
bool loadReplay(const std::string& path, Replay& out);

}  // namespace movement
