#include "ui/strafe_trainer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {

constexpr float kGaugeMin = 1.0f;
constexpr float kGaugeMax = 200.0f;
constexpr float kGaugePerfect = 100.0f;
constexpr float kTauSlow = 0.150f;
constexpr int kBarWidth = 21;

float normalizeAngle(float angle) {
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

// strafetrainer.sp's GetTieredColour: a 5-stop gradient white->green->red(ish)
// centered on the perfect value, same stops/colors.
void tieredColor(float value, float& r, float& g, float& b) {
    float dist = std::abs(value - kGaugePerfect);
    float maxDist = (value < kGaugePerfect) ? (kGaugePerfect - kGaugeMin) : (kGaugeMax - kGaugePerfect);
    float t = std::clamp(maxDist > 0.0f ? dist / maxDist : 0.0f, 0.0f, 1.0f);

    constexpr float cR[5] = {255, 0, 0, 255, 255};
    constexpr float cG[5] = {255, 255, 255, 165, 0};
    constexpr float cB[5] = {255, 255, 0, 0, 0};
    constexpr float stops[5] = {0.0f, 0.15f, 0.35f, 0.65f, 1.0f};

    int i = 0;
    while (i < 4 && t > stops[i + 1]) i++;
    float segT = (stops[i + 1] > stops[i]) ? (t - stops[i]) / (stops[i + 1] - stops[i]) : 0.0f;
    r = (cR[i] + (cR[i + 1] - cR[i]) * segT) / 255.0f;
    g = (cG[i] + (cG[i + 1] - cG[i]) * segT) / 255.0f;
    b = (cB[i] + (cB[i + 1] - cB[i]) * segT) / 255.0f;
}

}  // namespace

void tickStrafeTrainer(StrafeTrainerState& state, float currentYawDeg, float horizontalSpeed, float maxSpeed,
                        float airSpeedCap, float dt, bool airborne, bool strafing) {
    if (!state.hasLastYaw) {
        state.lastYawDeg = currentYawDeg;
        state.hasLastYaw = true;
        return;
    }

    float deltaYaw = normalizeAngle(currentYawDeg - state.lastYawDeg);
    state.lastYawDeg = currentYawDeg;

    if (!airborne || !strafing || horizontalSpeed <= 0.0f) {
        return;
    }

    float wishSpeed = std::min(maxSpeed, airSpeedCap);
    float idealAngleDeg = std::atan(wishSpeed / horizontalSpeed) * (180.0f / 3.14159265f);
    if (idealAngleDeg < 0.01f) {
        return;
    }

    float actualAngleDeg = std::abs(deltaYaw);
    float rawRatio = std::clamp((actualAngleDeg / idealAngleDeg) * 100.0f, kGaugeMin, kGaugeMax);

    float alpha = 1.0f - std::exp(-dt / kTauSlow);
    state.displayValue += alpha * (rawRatio - state.displayValue);
}

void drawStrafeTrainerBar(const StrafeTrainerState& state, bool airborne) {
    if (!airborne) {
        return;
    }

    float value = state.displayValue;
    int pos = static_cast<int>(std::round(((value - kGaugeMin) / (kGaugeMax - kGaugeMin)) * (kBarWidth - 1)));
    pos = std::clamp(pos, 0, kBarWidth - 1);

    char bar[kBarWidth + 1];
    for (int i = 0; i < kBarWidth; ++i) {
        bar[i] = (i == kBarWidth / 2) ? '|' : '-';
    }
    bar[pos] = 'o';
    bar[kBarWidth] = '\0';

    float r, g, b;
    tieredColor(value, r, g, b);

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.65f), ImGuiCond_Always,
                             ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##strafe_trainer", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(r, g, b, 1.0f));
    ImGui::SetWindowFontScale(1.3f);
    float textWidth = ImGui::CalcTextSize(bar).x;
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - textWidth) * 0.5f);
    ImGui::TextUnformatted(bar);

    char pct[16];
    std::snprintf(pct, sizeof(pct), "%.1f%%", value);
    float pctWidth = ImGui::CalcTextSize(pct).x;
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - pctWidth) * 0.5f);
    ImGui::TextUnformatted(pct);
    ImGui::PopStyleColor();
    ImGui::End();
}

}  // namespace ui
