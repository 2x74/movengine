#include "ui/settings_panel.h"

#include <imgui.h>

#include <algorithm>

namespace ui {

void drawSettingsPanel(console::CvarRegistry& cvars, bool& open) {
    if (!open) {
        return;
    }

    if (ImGui::Begin("settings", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        for (auto& cvar : cvars.all()) {
            ImGui::PushID(cvar.name.c_str());
            // slider to drag, box beside it to type an exact value
            float value = cvar.value;
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat("##slider", &value, cvar.minValue, cvar.maxValue)) {
                cvars.set(cvar.name, value);
            }
            ImGui::SameLine();
            float typed = cvar.value;
            ImGui::SetNextItemWidth(90.0f);
            ImGui::InputFloat("##value", &typed, 0.0f, 0.0f, "%.3f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                cvars.set(cvar.name, std::clamp(typed, cvar.minValue, cvar.maxValue));
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(cvar.name.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n(default %g, %g to %g)", cvar.description.c_str(), cvar.defaultValue,
                                  cvar.minValue, cvar.maxValue);
            }
            if (cvar.value != cvar.defaultValue) {
                ImGui::SameLine();
                if (ImGui::SmallButton("reset")) cvars.set(cvar.name, cvar.defaultValue);
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

}  // namespace ui
