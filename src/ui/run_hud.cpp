#include "ui/run_hud.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>

namespace ui {

std::string formatRunTime(float seconds) {
    long cs = std::lround(static_cast<double>(seconds) * 100.0);  // whole centiseconds, so 59.996 can't print "60.00"
    long h = cs / 360000, m = cs / 6000 % 60, sec = cs / 100 % 60, frac = cs % 100;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld.%02ld", h, m, sec, frac);
    else if (m > 0) std::snprintf(buf, sizeof(buf), "%ld:%02ld.%02ld", m, sec, frac);
    else std::snprintf(buf, sizeof(buf), "%ld.%02ld", sec, frac);
    return buf;
}

void drawRunHud(const movement::TimerState& timer, const std::string& styleName, float horizontalSpeed,
                float bestSeconds, float lastDelta) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.78f), ImGuiCond_Always,
                             ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##run_hud", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove);

    auto centeredText = [](const char* text) {
        float w = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - w) * 0.5f);
        ImGui::TextUnformatted(text);
    };

    centeredText(styleName.c_str());

    char buf[96];
    if (timer.running) {
        std::snprintf(buf, sizeof(buf), "Time: %s", formatRunTime(timer.elapsedSeconds).c_str());
    } else if (timer.lastRunTimeSeconds >= 0.0f && !std::isnan(lastDelta)) {
        std::snprintf(buf, sizeof(buf), "Time: %s (%+.2f)", formatRunTime(timer.lastRunTimeSeconds).c_str(),
                      lastDelta);
    } else if (timer.lastRunTimeSeconds >= 0.0f) {
        std::snprintf(buf, sizeof(buf), "Time: %s", formatRunTime(timer.lastRunTimeSeconds).c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "Time: --");
    }
    centeredText(buf);

    std::snprintf(buf, sizeof(buf), "Jumps: %d", timer.jumps);
    centeredText(buf);

    float syncPct = timer.airTicks > 0 ? (100.0f * static_cast<float>(timer.goodAirTicks) / timer.airTicks) : 0.0f;
    std::snprintf(buf, sizeof(buf), "Sync: %.1f%%", syncPct);
    centeredText(buf);

    std::snprintf(buf, sizeof(buf), "Speed: %.0f", horizontalSpeed);
    centeredText(buf);

    if (bestSeconds >= 0.0f) {
        std::snprintf(buf, sizeof(buf), "PB: %s", formatRunTime(bestSeconds).c_str());
        centeredText(buf);
    }

    ImGui::End();
}

}  // namespace ui
