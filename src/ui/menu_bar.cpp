#include "ui/menu_bar.h"

#include <imgui.h>

namespace ui {

void drawMenuBar(MenuBarState& state) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("file")) {
        if (ImGui::MenuItem("open map...")) {
            state.openMapClicked = true;
        }
        if (ImGui::MenuItem("exec config...")) {
            state.execConfigClicked = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("run a cs:s-style .cfg file (cvars it recognizes get applied, everything else is logged and skipped)");
        }
        if (ImGui::MenuItem("cs:s folder...")) {
            state.mountCssClicked = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("point at your Counter-Strike Source install to use its textures (found automatically through Steam when possible)");
        }
        if (ImGui::MenuItem("texture pack...")) {
            state.loadPackClicked = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("use a CS:S texture pack (.zip) instead of an install -- or just put css_textures.zip next to the app");
        }
        if (ImGui::MenuItem("export cs:s textures...")) {
            state.exportPackClicked = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("pack every CS:S world texture into one .zip, to use on a computer without CS:S (like a laptop)");
        }
        if (ImGui::MenuItem("open in editor")) {
            state.openEditorClicked = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("opens this map in movengine_editor (a .bsp gets decompiled). press F5 there to play your edits live.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("exit")) {
            state.exitClicked = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("edit")) {
        if (ImGui::MenuItem("keybinds...")) {
            state.showKeybindsWindow = true;
        }
        if (ImGui::MenuItem("settings...")) {
            state.showSettingsWindow = true;
        }
        if (ImGui::MenuItem("zones...")) {
            state.showZonesWindow = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("place, move or delete start/end zones, and add bonuses");
        }
        ImGui::Separator();
        ImGui::MenuItem("nullify a/d", nullptr, &state.nullAD);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("holding A and D moves you the way you pressed last,\ninstead of the two cancelling out");
        }
        ImGui::MenuItem("release w upon jumping", nullptr, &state.releaseWOnJump);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("W stops counting while jump is held");
        }
        ImGui::MenuItem("recorrect momentum through teleports", nullptr, &state.recorrectTeleportMomentum);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Source keeps the direction you entered with, which can spit you out\n"
                              "backwards. This turns your speed to face the way the destination does.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("sounds...")) {
            state.showSoundsWindow = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("pick the sounds for finishing, a new personal best, dying, and more");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("styles")) {
        for (size_t i = 0; i < state.styleNames.size(); ++i) {
            bool selected = (state.selectedStyle == static_cast<int>(i));
            if (ImGui::MenuItem(state.styleNames[i].c_str(), nullptr, selected)) {
                state.selectedStyle = static_cast<int>(i);
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("times")) {
        ImGui::TextDisabled("%s", state.timesHeader.c_str());
        ImGui::Separator();
        if (state.times.empty()) ImGui::TextDisabled("no times yet");
        for (size_t i = 0; i < state.times.size(); ++i) {
            const auto& row = state.times[i];
            ImGui::PushID(static_cast<int>(i));
            if (row.current) {
                ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.5f, 1.0f), "%s", row.label.c_str());
            } else {
                ImGui::TextUnformatted(row.label.c_str());
            }
            if (ImGui::IsItemHovered() && !row.details.empty()) ImGui::SetTooltip("%s", row.details.c_str());
            if (row.canApply && !row.current) {
                ImGui::SameLine();
                if (ImGui::SmallButton("use these settings")) state.applyTimeClicked = static_cast<int>(i);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("switch to the style and settings this time was set with:\n%s", row.details.c_str());
                }
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("view")) {
        ImGui::MenuItem("jhud", nullptr, &state.showJhud);
        ImGui::Separator();
        ImGui::MenuItem("show clips", nullptr, &state.showClips);
        ImGui::MenuItem("show triggers", nullptr, &state.showTriggers);
        ImGui::Separator();
        if (ImGui::MenuItem("replay best run", "p", false, state.hasReplay)) {
            state.replayClicked = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("help")) {
        if (ImGui::MenuItem("about")) {
            state.showAboutWindow = true;
        }
        ImGui::EndMenu();
    }

    ImGui::SameLine(ImGui::GetWindowWidth() - 160.0f);
    ImGui::TextDisabled("[tab] release mouse");

    ImGui::EndMainMenuBar();

    if (state.showAboutWindow) {
        if (ImGui::Begin("about", &state.showAboutWindow, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("movengine");
            ImGui::Text("standalone bhop/surf practice engine");
        }
        ImGui::End();
    }
}

}  // namespace ui
