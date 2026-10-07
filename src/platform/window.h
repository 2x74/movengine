#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <vector>

namespace platform {

// owns the sdl window + gl 3.3 core context lifecycle
class Window {
public:
    bool init(const char* title, int width, int height);
    void shutdown();

    void swap();
    // pumps the event queue, sets quitRequested when the user closes the window. accumulates relative mouse motion for the frame (read via consumeMouseDelta). if onEvent is set it's called for every event (after Window's own handling) so callers can route events to imgui, keybind capture etc without Window needing to know those systems exist
    void pollEvents(bool& quitRequested, const std::function<void(const SDL_Event&)>& onEvent = nullptr);

    // one mouse report, with the timestamp the device's event carried instead of the time we happened to poll it
    struct MouseSample {
        float dx = 0.0f, dy = 0.0f;
        Uint64 timestampNs = 0;
    };

    void setRelativeMouseMode(bool enabled);
    // borderless desktop fullscreen (sdl picks the mode), toggled with f11
    void setFullscreen(bool enabled);
    // 0 = off, 1 = on, -1 = adaptive. False if the driver refused it.
    bool setVsync(int interval) { return SDL_GL_SetSwapInterval(interval); }
    // asked of sdl instead of remembered. a tracked bool and the real window drift apart whenever a request is refused or the enter/leave event the tracking relies on doesn't arrive, and then the next toggle asks for the state it's already in and looks like a dead key
    bool fullscreen() const;
    // returns accumulated mouse motion since the last call and resets it to zero
    void consumeMouseDelta(float& dx, float& dy);
    // hands over the individual reports behind that sum and clears them, so a caller can place each one in the tick it actually happened in instead of spreading the frame's total evenly. swaps, so `out`'s capacity is reused
    void consumeMouseSamples(std::vector<MouseSample>& out);

    // the framebuffer's size in pixels, which is what glReadPixels works in. on a hidpi display this is larger than width()/height(), which are in window coordinates
    void drawableSize(int& w, int& h) const { SDL_GetWindowSizeInPixels(window_, &w, &h); }

    int width() const { return width_; }
    int height() const { return height_; }

    SDL_Window* handle() const { return window_; }
    SDL_GLContext glContext() const { return glContext_; }

    // shrinks the window to fit the display it's on, frame included, and centres it. called on creation, safe to call again
    void fitToDisplay();

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    float mouseDeltaX_ = 0.0f;
    float mouseDeltaY_ = 0.0f;
    // what setRelativeMouseMode was last asked for, re asserted whenever the window takes focus again (sdl drops relative mode on focus loss)
    bool relativeMouseWanted_ = false;
    // bounded: a caller that never consumes these (the editor) must not grow this without limit. 2048 is ~2s of a 1000Hz mouse
    static constexpr size_t kMaxMouseSamples = 2048;
    std::vector<MouseSample> mouseSamples_;
};

}  // namespace platform
