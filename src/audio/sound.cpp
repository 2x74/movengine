#include "audio/sound.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

extern "C" {
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);
}

namespace audio {

namespace {

bool startsWith(const unsigned char* data, size_t size, const char* magic) {
    size_t n = std::strlen(magic);
    return size >= n && std::memcmp(data, magic, n) == 0;
}

Pcm fromFloats(float* frames, unsigned channels, unsigned rate, unsigned long long frameCount) {
    Pcm pcm;
    pcm.channels = static_cast<int>(channels);
    pcm.sampleRate = static_cast<int>(rate);
    pcm.samples.assign(frames, frames + frameCount * channels);
    return pcm;
}

}  // namespace

std::optional<Pcm> decodeMemory(const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    if (startsWith(bytes, size, "RIFF")) {
        unsigned channels = 0, rate = 0;
        drwav_uint64 frames = 0;
        float* out = drwav_open_memory_and_read_pcm_frames_f32(data, size, &channels, &rate, &frames, nullptr);
        if (!out) return std::nullopt;
        Pcm pcm = fromFloats(out, channels, rate, frames);
        drwav_free(out, nullptr);
        return pcm;
    }
    if (startsWith(bytes, size, "fLaC")) {
        unsigned channels = 0, rate = 0;
        drflac_uint64 frames = 0;
        float* out = drflac_open_memory_and_read_pcm_frames_f32(data, size, &channels, &rate, &frames, nullptr);
        if (!out) return std::nullopt;
        Pcm pcm = fromFloats(out, channels, rate, frames);
        drflac_free(out, nullptr);
        return pcm;
    }
    if (startsWith(bytes, size, "OggS")) {
        int channels = 0, rate = 0;
        short* out = nullptr;
        int frames = stb_vorbis_decode_memory(bytes, static_cast<int>(size), &channels, &rate, &out);
        if (frames <= 0 || !out) return std::nullopt;
        Pcm pcm;
        pcm.channels = channels;
        pcm.sampleRate = rate;
        pcm.samples.resize(static_cast<size_t>(frames) * channels);
        for (size_t i = 0; i < pcm.samples.size(); ++i) pcm.samples[i] = out[i] / 32768.0f;
        std::free(out);
        return pcm;
    }
    // mp3 has no reliable magic (id3 tag or a bare frame sync) so it's the fallback
    drmp3_config config{};
    drmp3_uint64 frames = 0;
    float* out = drmp3_open_memory_and_read_pcm_frames_f32(data, size, &config, &frames, nullptr);
    if (!out || frames == 0) {
        if (out) drmp3_free(out, nullptr);
        return std::nullopt;
    }
    Pcm pcm = fromFloats(out, config.channels, config.sampleRate, frames);
    drmp3_free(out, nullptr);
    return pcm;
}

std::optional<Pcm> decodeFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.empty()) return std::nullopt;
    return decodeMemory(data.data(), data.size());
}

struct SoundPlayer::Voice {
    SDL_AudioStream* stream = nullptr;
};

SoundPlayer::SoundPlayer() = default;
SoundPlayer::~SoundPlayer() { shutdown(); }

bool SoundPlayer::init() {
    if (device_) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "audio: no audio (%s)\n", SDL_GetError());
        return false;
    }
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!device_) {
        std::fprintf(stderr, "audio: couldn't open the output device (%s)\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    // a few voices so a pb sound doesn't cut off a teleport sound
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 44100};
    for (int i = 0; i < 4; ++i) {
        auto voice = std::make_unique<Voice>();
        voice->stream = SDL_CreateAudioStream(&spec, nullptr);
        if (voice->stream && SDL_BindAudioStream(device_, voice->stream)) {
            voices_.push_back(std::move(voice));
        } else if (voice->stream) {
            SDL_DestroyAudioStream(voice->stream);
        }
    }
    std::printf("audio: %s, %zu voices\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
                voices_.size());
    return true;
}

void SoundPlayer::shutdown() {
    for (auto& v : voices_) SDL_DestroyAudioStream(v->stream);
    voices_.clear();
    if (device_) {
        SDL_CloseAudioDevice(device_);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        device_ = 0;
    }
}

const Pcm* SoundPlayer::load(const std::filesystem::path& path) {
    std::string key = path.generic_string();
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        auto pcm = decodeFile(path);
        if (!pcm) std::fprintf(stderr, "audio: couldn't play %s (missing, or not mp3/wav/flac/ogg)\n", key.c_str());
        it = cache_.emplace(key, std::move(pcm)).first;
    }
    return it->second ? &*it->second : nullptr;
}

void SoundPlayer::forget(const std::filesystem::path& path) { cache_.erase(path.generic_string()); }

void SoundPlayer::play(const std::filesystem::path& path, float volume) {
    if (!device_ || voices_.empty() || path.empty() || volume <= 0.0f) return;
    const Pcm* pcm = load(path);
    if (!pcm || pcm->samples.empty()) return;

    // one sound at a time: a new one cuts off whatever's still playing instead of layering. jump fires on every hop so with autobhop a pool would stack a dozen overlapping copies of the same clip within a second and turn into noise
    for (auto& v : voices_) SDL_ClearAudioStream(v->stream);
    Voice* voice = voices_.front().get();
    SDL_AudioSpec src{SDL_AUDIO_F32, pcm->channels, pcm->sampleRate};
    SDL_SetAudioStreamFormat(voice->stream, &src, nullptr);
    SDL_SetAudioStreamGain(voice->stream, volume);
    SDL_PutAudioStreamData(voice->stream, pcm->samples.data(), static_cast<int>(pcm->samples.size() * sizeof(float)));
}

const char* soundEventKey(SoundEvent e) {
    switch (e) {
        case SoundEvent::Finish: return "finish";
        case SoundEvent::PersonalBest: return "personal_best";
        case SoundEvent::Death: return "death";
        case SoundEvent::Teleport: return "teleport";
        case SoundEvent::RunStart: return "run_start";
        case SoundEvent::Restart: return "restart";
        case SoundEvent::Jump: return "jump";
        case SoundEvent::Count: break;
    }
    return "";
}

const char* soundEventLabel(SoundEvent e) {
    switch (e) {
        case SoundEvent::Finish: return "finish";
        case SoundEvent::PersonalBest: return "new personal best";
        case SoundEvent::Death: return "death (trigger_hurt / fell out of the map)";
        case SoundEvent::Teleport: return "teleported by the map";
        case SoundEvent::RunStart: return "run start (left the start zone)";
        case SoundEvent::Restart: return "restart";
        case SoundEvent::Jump: return "every jump";
        case SoundEvent::Count: break;
    }
    return "";
}

SoundConfig defaultSoundConfig() {
    SoundConfig config;
    config.paths[static_cast<int>(SoundEvent::Finish)] = "sounds/fr_1.mp3";
    config.paths[static_cast<int>(SoundEvent::PersonalBest)] = "sounds/pb_2.mp3";
    return config;
}

void loadSoundConfig(const std::string& file, SoundConfig& config) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string key;
        if (!(iss >> key)) continue;
        std::string rest;
        std::getline(iss >> std::ws, rest);
        if (key == "volume") {
            config.volume = std::clamp(std::strtof(rest.c_str(), nullptr), 0.0f, 1.0f);
            continue;
        }
        for (int i = 0; i < kSoundEventCount; ++i) {
            if (key == soundEventKey(static_cast<SoundEvent>(i))) {
                config.paths[i] = rest == "none" ? "" : rest;
            }
        }
    }
}

bool saveSoundConfig(const std::string& file, const SoundConfig& config) {
    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    out << "volume " << config.volume << '\n';
    for (int i = 0; i < kSoundEventCount; ++i) {
        out << soundEventKey(static_cast<SoundEvent>(i)) << ' ' << (config.paths[i].empty() ? "none" : config.paths[i])
            << '\n';
    }
    return true;
}

std::filesystem::path resolveSoundPath(const std::string& path, const std::filesystem::path& appDir) {
    if (path.empty()) return {};
    std::filesystem::path p(reinterpret_cast<const char8_t*>(path.c_str()));  // UTF-8, also on Windows
    return p.is_absolute() ? p : appDir / p;
}

}  // namespace audio
