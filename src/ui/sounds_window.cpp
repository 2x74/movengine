#include "ui/sounds_window.h"

#include <imgui.h>

#include <string>

#include "platform/file_dialog.h"

namespace ui {

namespace {

// relative to the app folder when the file is inside it
std::string storedPath(const std::string& chosen, const std::filesystem::path& appDir) {
    std::filesystem::path p(reinterpret_cast<const char8_t*>(chosen.c_str()));
    std::error_code ec;
    if (!appDir.empty()) {
        std::filesystem::path rel = std::filesystem::relative(p, appDir, ec);
        if (!ec && !rel.empty() && rel.native()[0] != '.') {
            auto u8 = rel.generic_u8string();
            return std::string(u8.begin(), u8.end());
        }
    }
    return chosen;
}

}  // namespace

SoundsWindowResult drawSoundsWindow(audio::SoundConfig& config, const std::filesystem::path& appDir, bool& open) {
    SoundsWindowResult result;
    if (!open) return result;
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("sounds", &open)) {
        ImGui::End();
        return result;
    }
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("volume", &config.volume, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) result.changed = true;
    ImGui::TextDisabled("mp3, wav, ogg or flac. put your own in the sounds folder next to the app, or browse to any file.");
    ImGui::Separator();

    audio::SoundConfig defaults = audio::defaultSoundConfig();
    for (int i = 0; i < audio::kSoundEventCount; ++i) {
        auto event = static_cast<audio::SoundEvent>(i);
        ImGui::PushID(i);
        ImGui::TextUnformatted(audio::soundEventLabel(event));
        ImGui::Indent();
        std::string& path = config.paths[i];
        if (path.empty()) {
            ImGui::TextDisabled("(none)");
        } else {
            ImGui::TextUnformatted(path.c_str());
        }
        if (ImGui::SmallButton("browse...")) {
            if (auto chosen = platform::chooseSoundFile()) {
                path = storedPath(*chosen, appDir);
                result.changed = true;
            }
        }
        if (!path.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("test")) result.testEvent = i;
            ImGui::SameLine();
            if (ImGui::SmallButton("none")) {
                path.clear();
                result.changed = true;
            }
        }
        if (path != defaults.paths[i]) {
            ImGui::SameLine();
            if (ImGui::SmallButton("default")) {
                path = defaults.paths[i];
                result.changed = true;
            }
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
    ImGui::End();
    return result;
}

}  // namespace ui
