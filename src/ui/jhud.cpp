#include "ui/jhud.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace ui {

namespace {

// longer than this on the ground ends the bhop chain so the next jump counts as jump 1 again. a few ticks of slack for scroll/legit styles
constexpr float kChainBreakSeconds = 0.1f;
constexpr float kVisibleSeconds = 2.0f;
constexpr float kFadeSeconds = 0.5f;

ImVec4 gainColor(float gainPct) {
    if (gainPct >= 90.0f) return ImVec4(0.75f, 0.45f, 1.0f, 1.0f);  // purple
    if (gainPct >= 80.0f) return ImVec4(0.3f, 0.85f, 1.0f, 1.0f);   // cyan
    if (gainPct >= 70.0f) return ImVec4(0.35f, 1.0f, 0.4f, 1.0f);   // green
    if (gainPct >= 60.0f) return ImVec4(1.0f, 0.65f, 0.2f, 1.0f);   // orange
    return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);                          // red
}

}  // namespace

void resetJhud(JhudState& state) {
    state = JhudState{};
}

void tickJhud(JhudState& state, bool justJumped, bool grounded, float horizontalSpeed, float airGain,
              float airGainMax, bool speedGainedInAir, float dt) {
    state.sinceLastJump += dt;

    if (justJumped) {
        if (state.groundedTime > kChainBreakSeconds) {
            state.jumps = 0;
        }
        // stats describe the airtime that just ended, so jump 1 has none
        state.hasStats = state.jumps > 0 && state.gainMaxSum > 0.0f;
        if (state.hasStats) {
            state.gainPct = 100.0f * state.gainSum / state.gainMaxSum;
            state.syncPct = state.airTicks > 0 ? 100.0f * state.syncedTicks / state.airTicks : 0.0f;
        }
        state.jumps++;
        state.takeoffSpeed = horizontalSpeed;
        state.sinceLastJump = 0.0f;
        state.groundedTime = 0.0f;
        state.gainSum = state.gainMaxSum = 0.0f;
        state.airTicks = state.syncedTicks = 0;
        return;
    }

    if (grounded) {
        state.groundedTime += dt;
        return;
    }
    state.groundedTime = 0.0f;
    state.gainSum += airGain;
    state.gainMaxSum += airGainMax;
    state.airTicks++;
    if (speedGainedInAir) {
        state.syncedTicks++;
    }
}

void drawJhud(const JhudState& state) {
    if (state.jumps == 0 || state.sinceLastJump > kVisibleSeconds) {
        return;
    }
    float alpha = std::clamp((kVisibleSeconds - state.sinceLastJump) / kFadeSeconds, 0.0f, 1.0f);

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.55f), ImGuiCond_Always,
                             ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::Begin("##jhud", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove);

    auto centered = [](const char* text, const ImVec4& color) {
        float w = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - w) * 0.5f);
        ImGui::TextColored(color, "%s", text);
    };

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d: %.0f", state.jumps, state.takeoffSpeed);
    centered(buf, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    if (state.hasStats) {
        std::snprintf(buf, sizeof(buf), "%.1f%%  %.0f%% sync", state.gainPct, state.syncPct);
        centered(buf, gainColor(state.gainPct));
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace ui
