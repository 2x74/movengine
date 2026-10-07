#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace audio {

// decoded audio, interleaved float samples
struct Pcm {
    int channels = 0;
    int sampleRate = 0;
    std::vector<float> samples;
};

// .mp3, .wav, .flac or .ogg (told apart by content, not extension).
std::optional<Pcm> decodeFile(const std::filesystem::path& path);
std::optional<Pcm> decodeMemory(const void* data, size_t size);

// short one shot sounds on the default output device. everything here is a no op when there's no audio device so callers never need to check
class SoundPlayer {
public:
    SoundPlayer();
    ~SoundPlayer();
    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;

    bool init();
    void shutdown();
    bool available() const { return device_ != 0; }

    // decoded once and cached, a file that won't decode is reported once and then stays silent
    void play(const std::filesystem::path& path, float volume);
    void forget(const std::filesystem::path& path);  // re-read it next time (the file was replaced)

private:
    struct Voice;
    const Pcm* load(const std::filesystem::path& path);

    unsigned device_ = 0;
    std::vector<std::unique_ptr<Voice>> voices_;
    size_t nextVoice_ = 0;
    std::unordered_map<std::string, std::optional<Pcm>> cache_;
};

// things that can play a sound. each one's sound is picked in edit > sounds... (or left silent)
enum class SoundEvent { Finish, PersonalBest, Death, Teleport, RunStart, Restart, Jump, Count };
constexpr int kSoundEventCount = static_cast<int>(SoundEvent::Count);

const char* soundEventKey(SoundEvent e);    // "personal_best", as saved
const char* soundEventLabel(SoundEvent e);  // "new personal best", as shown

struct SoundConfig {
    // empty = silent. relative paths are from the folder the app is in
    std::array<std::string, kSoundEventCount> paths;
    float volume = 0.7f;

    const std::string& path(SoundEvent e) const { return paths[static_cast<int>(e)]; }
};

// fr_1 for a normal finish, pb_2 for a personal best, the rest silent.
SoundConfig defaultSoundConfig();
// one "event path" line per event plus "volume v", events missing from the file keep their defaults so new events get theirs
void loadSoundConfig(const std::string& file, SoundConfig& config);
bool saveSoundConfig(const std::string& file, const SoundConfig& config);
// a configured path made absolute against `appDir` if it's relative
std::filesystem::path resolveSoundPath(const std::string& path, const std::filesystem::path& appDir);

}  // namespace audio
