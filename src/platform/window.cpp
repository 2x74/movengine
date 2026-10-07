#include "platform/window.h"

#include <glad/gl.h>

#include <algorithm>
#include <cstdio>

namespace platform {

bool Window::init(const char* title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    // the size asked for is a preference not a promise. clamp it to the display before creating the window: on a screen smaller than the request, and on windows at 125%/150% display scaling (window size is in logical units, so the editor's 1600x900 on a 150% scaled 1920x1080 monitor asks for 2400x1350 physical pixels) an unclamped window opens larger than the desktop with its title bar off screen, where it can be neither moved nor resized back
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {
        width = std::min(width, usable.w);
        height = std::min(height, usable.h);
    }

    width_ = width;
    height_ = height;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    window_ = SDL_CreateWindow(title, width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) {
        SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return false;
    }

    // the frame isn't part of the size above and is only measurable once the window exists, so take the title bar and borders out of the budget too, then centre what's left
    fitToDisplay();

    SDL_GL_MakeCurrent(window_, glContext_);
    // off by default: vsync pins the frame rate to the display's refresh, which makes fps_max a no op above it and adds up to a frame of latency between a mouse move and seeing it. both matter here
    SDL_GL_SetSwapInterval(0);

    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        SDL_Log("gladLoadGL failed");
        return false;
    }

    std::printf("GL vendor: %s\n", reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    std::printf("GL renderer: %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    std::printf("GL version: %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    return true;
}

void Window::fitToDisplay() {
    if (!window_) return;
    SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
    if (!display) display = SDL_GetPrimaryDisplay();
    SDL_Rect usable;
    if (!SDL_GetDisplayUsableBounds(display, &usable) || usable.w <= 0 || usable.h <= 0) return;

    // zeros when the platform can't say, which just means no allowance
    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(window_, &top, &left, &bottom, &right);

    int maxWidth = usable.w - (left + right);
    int maxHeight = usable.h - (top + bottom);
    int width = maxWidth > 0 ? std::min(width_, maxWidth) : width_;
    int height = maxHeight > 0 ? std::min(height_, maxHeight) : height_;
    if (width != width_ || height != height_) {
        SDL_SetWindowSize(window_, width, height);
        width_ = width;
        height_ = height;
    }
    // undefined placement can still put a window that does fit partly past the edge, so put it somewhere the title bar is definitely reachable
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void Window::shutdown() {
    if (glContext_) {
        SDL_GL_DestroyContext(glContext_);
        glContext_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

void Window::swap() {
    SDL_GL_SwapWindow(window_);
}

void Window::setRelativeMouseMode(bool enabled) {
    // tried also calling SDL_SetWindowMouseGrab() here to force confinement on top of relative mode, since the cursor could still wander on a multi monitor x11 setup. that backfired badly: it's a second separate grab mechanism layered on one sdl3 already manages internally in sync with focus events, and the two fighting over the grab made keyboard input to the window unreliable (showed up as "can't walk", wasd presses silently not landing) and could shove focus to another window entirely. relative mode alone is the sdl3 native way to do this, if cursor wandering comes back fix it by re asserting relative mode on SDL_EVENT_WINDOW_FOCUS_GAINED, not by adding a second grab call
    relativeMouseWanted_ = enabled;
    if (SDL_SetWindowRelativeMouseMode(window_, enabled) || !enabled) return;
    // silently ignoring this is how you end up with a window that renders fine and ticks fine but swallows every wasd press and never turns the view. on x11 relative mode needs xinput2, which sdl only compiles in when libxi's headers are present at ITS build time, so a from source sdl on a machine without libxi-dev/libxfixes-dev silently ships without mouse capture. say so once, with the fix
    static bool reported = false;
    if (reported) return;
    reported = true;
    std::fprintf(stderr,
                 "\nmouse capture unavailable: %s\n"
                 "  Mouse look and keyboard movement will not work.\n"
                 "  On X11 this usually means SDL was built without XInput2.\n"
                 "  Install the headers and rebuild from scratch:\n"
                 "    sudo apt install libxi-dev libxfixes-dev libxcursor-dev\n"
                 "    ./rebuild.sh --clean\n\n",
                 SDL_GetError());
}

bool Window::fullscreen() const {
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

void Window::setFullscreen(bool enabled) {
    // a null fullscreen mode means borderless desktop fullscreen instead of a video mode change, which is what you want for a game you alt tab out of
    SDL_SetWindowFullscreenMode(window_, nullptr);
    if (!SDL_SetWindowFullscreen(window_, enabled)) {
        std::fprintf(stderr, "couldn't change fullscreen: %s\n", SDL_GetError());
        return;
    }
    // this is only a *request* and the window manager is free to refuse it. believing it outright left the flag disagreeing with the real window, and the next toggle then asked for the state it was already in, so fullscreen looked like it needed two presses or stopped working. wait for the change, and take the truth from the enter/leave events in pollEvents either way
    SDL_SyncWindow(window_);
}

void Window::consumeMouseSamples(std::vector<MouseSample>& out) {
    out.clear();
    out.swap(mouseSamples_);
}

void Window::consumeMouseDelta(float& dx, float& dy) {
    dx = mouseDeltaX_;
    dy = mouseDeltaY_;
    mouseDeltaX_ = 0.0f;
    mouseDeltaY_ = 0.0f;
}

void Window::pollEvents(bool& quitRequested, const std::function<void(const SDL_Event&)>& onEvent) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            quitRequested = true;
        }
        if (event.type == SDL_EVENT_WINDOW_RESIZED) {
            width_ = event.window.data1;
            height_ = event.window.data2;
            glViewport(0, 0, width_, height_);
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            // sdl drops relative mode when the window loses focus and doesn't restore it, so a window that starts unfocused (launched from a terminal) or is alt tabbed away from comes back with no mouse look and no wasd. re assert what we asked for
            SDL_SetWindowRelativeMouseMode(window_, relativeMouseWanted_);
            mouseDeltaX_ = 0.0f;  // drop the jump from wherever the cursor was
            mouseDeltaY_ = 0.0f;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            mouseDeltaX_ += event.motion.xrel;
            mouseDeltaY_ += event.motion.yrel;
            if (mouseSamples_.size() < kMaxMouseSamples) {
                mouseSamples_.push_back({event.motion.xrel, event.motion.yrel, event.motion.timestamp});
            }
        }
        if (onEvent) {
            onEvent(event);
        }
    }
}

}  // namespace platform
