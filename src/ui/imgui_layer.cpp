#include "ui/imgui_layer.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

namespace ui {

bool ImGuiLayer::init(platform::Window& window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForOpenGL(window.handle(), window.glContext())) {
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        return false;
    }

    initialized_ = true;
    return true;
}

void ImGuiLayer::shutdown() {
    if (!initialized_) {
        return;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void ImGuiLayer::processEvent(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::newFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::render() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void keepWindowOnScreen(bool when, bool allowResize) {
    if (!when) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 areaPos = viewport->WorkPos;
    const ImVec2 areaSize = viewport->WorkSize;
    if (areaSize.x <= 0.0f || areaSize.y <= 0.0f) return;

    ImVec2 size = ImGui::GetWindowSize();
    if (allowResize) {
        ImVec2 fitted = size;
        if (fitted.x > areaSize.x) fitted.x = areaSize.x;
        if (fitted.y > areaSize.y) fitted.y = areaSize.y;
        if (fitted.x != size.x || fitted.y != size.y) {
            ImGui::SetWindowSize(fitted);
            size = fitted;
        }
    }

    const ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 fitted = pos;
    // right/bottom first, then left/top, so a panel still too big to fit ends up at the top left showing its controls instead of its far corner
    if (fitted.x + size.x > areaPos.x + areaSize.x) fitted.x = areaPos.x + areaSize.x - size.x;
    if (fitted.y + size.y > areaPos.y + areaSize.y) fitted.y = areaPos.y + areaSize.y - size.y;
    if (fitted.x < areaPos.x) fitted.x = areaPos.x;
    if (fitted.y < areaPos.y) fitted.y = areaPos.y;
    if (fitted.x != pos.x || fitted.y != pos.y) ImGui::SetWindowPos(fitted);
}

}  // namespace ui
