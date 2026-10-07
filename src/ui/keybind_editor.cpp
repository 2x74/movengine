#include "ui/keybind_editor.h"

#include <imgui.h>

namespace ui {

void drawKeybindEditor(KeybindRegistry& registry, bool& open) {
    if (!open) {
        return;
    }

    if (ImGui::Begin("keybinds", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("click rebind, then press any key");
        ImGui::Separator();

        for (const auto& bind : registry.all()) {
            ImGui::TextUnformatted(bind.description.c_str());
            ImGui::SameLine(220.0f);

            bool capturingThis = registry.capturingAction() == bind.action;
            if (capturingThis) {
                ImGui::TextDisabled("press a key...");
            } else {
                ImGui::Text("%s", SDL_GetScancodeName(bind.key));
                ImGui::SameLine();
                ImGui::PushID(bind.action.c_str());
                ImGui::BeginDisabled(registry.isCapturing());
                if (ImGui::SmallButton("rebind")) {
                    registry.beginRebind(bind.action);
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
}

}  // namespace ui
