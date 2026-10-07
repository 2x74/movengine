#include "ui/zones_window.h"

#include <imgui.h>

#include <algorithm>
#include <string>

namespace ui {

namespace {

// bounds typed or dragged inside out get sorted back, and the corner the zone's middle falls back to is kept inside it
void tidyZone(movement::Zone& zone) {
    glm::vec3 lo = glm::min(zone.min, zone.max);
    glm::vec3 hi = glm::max(zone.min, zone.max);
    zone.min = lo;
    zone.max = hi;
    zone.anchorPoint = glm::clamp(zone.anchorPoint, lo, hi);
    zone.anchorPoint.z = lo.z;
}

void zoneRow(const char* label, movement::TrackZones& track, int trackIndex, bool start, int creatingTrack,
             bool creatingStart, ZonesWindowResult& result) {
    movement::Zone& zone = start ? track.start : track.end;
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    ImGui::SameLine(60.0f);
    bool placing = creatingTrack == trackIndex && creatingStart == start;
    if (placing) {
        ImGui::TextDisabled("placing... (e at two corners)");
    } else {
        if (ImGui::SmallButton(zone.defined ? "replace" : "place")) {
            result.placeTrack = trackIndex;
            result.placeStart = start;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("walk to one corner and press e, then the opposite corner and press e");
        }
        if (zone.defined) {
            ImGui::SameLine();
            if (ImGui::SmallButton("delete")) {
                movement::removeZone(track, start);
                result.changed = true;
            }
        }
    }
    if (zone.defined) {
        ImGui::SetNextItemWidth(260.0f);
        ImGui::DragFloat3("min", &zone.min.x, 1.0f, -32768.0f, 32768.0f, "%.0f");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            tidyZone(zone);
            result.changed = true;
        }
        ImGui::SetNextItemWidth(260.0f);
        ImGui::DragFloat3("max", &zone.max.x, 1.0f, -32768.0f, 32768.0f, "%.0f");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            tidyZone(zone);
            result.changed = true;
        }
    }
    ImGui::PopID();
}

}  // namespace

ZonesWindowResult drawZonesWindow(movement::MapZones& zones, int currentTrack, int creatingTrack,
                                   bool creatingStart, bool& open) {
    ZonesWindowResult result;
    if (!open) return result;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("zones", &open)) {
        ImGui::End();
        return result;
    }
    ImGui::TextWrapped("drag a number to nudge a zone, or double-click it to type. deleting a start zone also clears "
                       "that track's set start (b).");
    ImGui::Separator();

    for (int t = 0; t < movement::kMaxTracks; ++t) {
        movement::TrackZones& track = zones.tracks[t];
        if (t != 0 && !track.used() && creatingTrack != t) continue;
        ImGui::PushID(t);
        std::string header = movement::trackName(t);
        if (t == currentTrack) header += "  (current)";
        if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (t != currentTrack && track.start.defined) {
                if (ImGui::SmallButton("go")) result.goTrack = t;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("switch to this track and restart onto it");
            }
            zoneRow("start", track, t, true, creatingTrack, creatingStart, result);
            zoneRow("end", track, t, false, creatingTrack, creatingStart, result);
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    int nextBonus = -1;
    for (int t = 1; t < movement::kMaxTracks; ++t) {
        if (!zones.tracks[t].used() && creatingTrack != t) {
            nextBonus = t;
            break;
        }
    }
    if (nextBonus < 0) {
        ImGui::TextDisabled("all %d bonuses are in use", movement::kMaxTracks - 1);
    } else if (ImGui::Button(("add " + movement::trackName(nextBonus)).c_str())) {
        result.placeTrack = nextBonus;
        result.placeStart = true;
    }
    ImGui::End();
    return result;
}

}  // namespace ui
