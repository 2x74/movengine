#include "movement/replay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace movement {

namespace {

constexpr char kMagic[8] = {'B', 'H', 'O', 'P', 'R', 'P', 'L', 'Y'};
constexpr uint32_t kVersion = 1;

void writeU32(std::ofstream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void writeF32(std::ofstream& out, float v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void writeString(std::ofstream& out, const std::string& s) {
    writeU32(out, static_cast<uint32_t>(s.size()));
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

bool readU32(std::ifstream& in, uint32_t& v) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(v))); }
bool readF32(std::ifstream& in, float& v) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(v))); }
bool readString(std::ifstream& in, std::string& s) {
    uint32_t n = 0;
    if (!readU32(in, n)) return false;
    if (n > (1u << 20)) return false;  // a style name is short; refuse to allocate on a corrupt length
    s.resize(n);
    return n == 0 || static_cast<bool>(in.read(s.data(), static_cast<std::streamsize>(n)));
}

// shortest way round the circle so a replay crossing 180 degrees doesn't spin the long way between two ticks
float lerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
    return a + d * t;
}

}  // namespace

ReplayFrame Replay::sample(float seconds) const {
    if (frames.empty()) return {};
    if (tickInterval <= 0.0f) return frames.front();
    float f = seconds / tickInterval;
    if (f <= 0.0f) return frames.front();
    if (f >= static_cast<float>(frames.size() - 1)) return frames.back();
    size_t i = static_cast<size_t>(f);
    float t = f - static_cast<float>(i);
    const ReplayFrame& a = frames[i];
    const ReplayFrame& b = frames[i + 1];
    ReplayFrame out;
    out.position = a.position + (b.position - a.position) * t;
    out.yawDeg = lerpAngle(a.yawDeg, b.yawDeg, t);
    out.pitchDeg = lerpAngle(a.pitchDeg, b.pitchDeg, t);
    out.ducking = t < 0.5f ? a.ducking : b.ducking;  // no sensible midpoint for a bool
    return out;
}

bool saveReplay(const std::string& path, const Replay& replay) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "couldn't write replay to %s\n", path.c_str());
        return false;
    }
    out.write(kMagic, sizeof(kMagic));
    writeU32(out, kVersion);
    writeF32(out, replay.seconds);
    writeF32(out, replay.tickInterval);
    writeU32(out, static_cast<uint32_t>(replay.track));
    writeString(out, replay.style);
    writeString(out, replay.settings);
    writeU32(out, static_cast<uint32_t>(replay.frames.size()));
    for (const ReplayFrame& f : replay.frames) {
        writeF32(out, f.position.x);
        writeF32(out, f.position.y);
        writeF32(out, f.position.z);
        writeF32(out, f.yawDeg);
        writeF32(out, f.pitchDeg);
        uint32_t flags = f.ducking ? 1u : 0u;
        writeU32(out, flags);
    }
    return static_cast<bool>(out);
}

bool loadReplay(const std::string& path, Replay& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[sizeof(kMagic)] = {};
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return false;
    uint32_t version = 0;
    if (!readU32(in, version) || version != kVersion) return false;

    Replay r;
    uint32_t track = 0, count = 0;
    if (!readF32(in, r.seconds) || !readF32(in, r.tickInterval) || !readU32(in, track)) return false;
    if (!readString(in, r.style) || !readString(in, r.settings)) return false;
    if (!readU32(in, count)) return false;
    r.track = static_cast<int>(track);
    // an hour at 100t is 360k frames, anything past that is a corrupt count not a run
    if (count > 1'000'000u) return false;
    r.frames.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        ReplayFrame f;
        uint32_t flags = 0;
        if (!readF32(in, f.position.x) || !readF32(in, f.position.y) || !readF32(in, f.position.z) ||
            !readF32(in, f.yawDeg) || !readF32(in, f.pitchDeg) || !readU32(in, flags)) {
            return false;  // truncated: leave `out` as it was
        }
        f.ducking = (flags & 1u) != 0;
        r.frames[i] = f;
    }
    out = std::move(r);
    return true;
}

}  // namespace movement
