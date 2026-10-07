#pragma once

#include <filesystem>

#include "audio/sound.h"

namespace ui {

struct SoundsWindowResult {
    int testEvent = -1;    // play this event's sound now
    bool changed = false;  // config edited: save it
};

// "edit > sounds...": a sound per event (finish, new pb, death, ...), each with browse / test / none / default, plus the volume. files chosen inside `appDir` are stored relative to it so the app folder can move
SoundsWindowResult drawSoundsWindow(audio::SoundConfig& config, const std::filesystem::path& appDir, bool& open);

}  // namespace ui
