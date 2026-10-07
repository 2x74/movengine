#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace ui {

struct Keybind {
    std::string action;
    std::string description;
    SDL_Scancode key;
    std::function<void()> onPressed;
};

// named, rebindable, persisted actions. not every action needs real behavior yet (zone/restart are reserved now, wired to real systems once the zone/timer features exist), the point is the key is rebindable and the binding survives a relaunch from day one, not that every action does something yet
class KeybindRegistry {
public:
    void add(const std::string& action, const std::string& description, SDL_Scancode defaultKey,
              std::function<void()> onPressed);

    // dispatches onPressed for whichever action currently owns this scancode, unless a rebind capture is in progress (captures instead of firing)
    void handleKeyDown(SDL_Scancode scancode);

    // puts the registry into "waiting for next key" mode for this action, the next handleKeyDown() call rebinds instead of dispatching
    void beginRebind(const std::string& action);
    bool isCapturing() const { return !capturingAction_.empty(); }
    const std::string& capturingAction() const { return capturingAction_; }

    const std::vector<Keybind>& all() const { return binds_; }

    // the key currently bound to an action, for binds that act while HELD (turnbinds) instead of on the press edge. SDL_SCANCODE_UNKNOWN if the action was never registered
    SDL_Scancode keyFor(const std::string& action) const {
        for (const auto& b : binds_)
            if (b.action == action) return b.key;
        return SDL_SCANCODE_UNKNOWN;
    }

    // simple "action=ScancodeName" text format, one per line
    bool load(const std::string& path);
    bool save(const std::string& path) const;

private:
    std::vector<Keybind> binds_;
    std::string capturingAction_;
};

}  // namespace ui
