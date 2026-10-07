#include "ui/keybinds.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace ui {

void KeybindRegistry::add(const std::string& action, const std::string& description,
                            SDL_Scancode defaultKey, std::function<void()> onPressed) {
    binds_.push_back({action, description, defaultKey, std::move(onPressed)});
}

void KeybindRegistry::handleKeyDown(SDL_Scancode scancode) {
    if (!capturingAction_.empty()) {
        for (auto& bind : binds_) {
            if (bind.action == capturingAction_) {
                bind.key = scancode;
                break;
            }
        }
        capturingAction_.clear();
        return;
    }

    for (const auto& bind : binds_) {
        if (bind.key == scancode && bind.onPressed) {
            bind.onPressed();
        }
    }
}

void KeybindRegistry::beginRebind(const std::string& action) {
    capturingAction_ = action;
}

bool KeybindRegistry::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string action = line.substr(0, eq);
        std::string keyName = line.substr(eq + 1);

        SDL_Scancode code = SDL_GetScancodeFromName(keyName.c_str());
        if (code == SDL_SCANCODE_UNKNOWN) {
            continue;
        }
        for (auto& bind : binds_) {
            if (bind.action == action) {
                bind.key = code;
                break;
            }
        }
    }
    return true;
}

bool KeybindRegistry::save(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "failed to write keybinds to %s\n", path.c_str());
        return false;
    }
    for (const auto& bind : binds_) {
        file << bind.action << "=" << SDL_GetScancodeName(bind.key) << "\n";
    }
    return true;
}

}  // namespace ui
