#pragma once

#include <SDL3/SDL.h>

#include "platform/window.h"

namespace ui {

// thin wrapper around dear imgui's sdl3 + opengl3 backends
class ImGuiLayer {
public:
    bool init(platform::Window& window);
    void shutdown();

    void processEvent(const SDL_Event& event);
    void newFrame();
    void render();

private:
    bool initialized_ = false;
};

// pulls the imgui window currently being built fully back inside the viewport, shrinking it first when it's larger than the viewport is.
//
// panel positions are remembered in imgui.ini, which outlives the window they were arranged in: a layout saved on a big screen reopened on a smaller one leaves panels hanging off the edge. imgui's own clamp only keeps a sliver of the title bar reachable, which is enough to drag a panel back but not enough to use it. call this just after Begin(), passing true only on the frames where the viewport size changed, so it fixes a stale layout without stopping you putting a panel half off screen on purpose.
//
// `allowResize` must be false for an AlwaysAutoResize window, whose size isn't ours to set
void keepWindowOnScreen(bool when, bool allowResize = true);

}  // namespace ui
