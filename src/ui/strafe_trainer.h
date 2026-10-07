#pragma once

namespace ui {

// ported directly from strafetrainer.sp on angelgirl.cloud (pulled to reference/vps-source/strafetrainer.sp), not approximated: same ideal angle formula (atan(wishspeed/speed), wishspeed capped at the same 30 u/s as everywhere else), same 1-200 gauge range with 100 = perfect centered under '|', same exponential smoothing. only the rainbow/custom color modes and hud position options were dropped as unnecessary for a single local player
struct StrafeTrainerState {
    float displayValue = 100.0f;
    float lastYawDeg = 0.0f;
    bool hasLastYaw = false;
};

// call every tick regardless of state (it needs to track yaw continuously to avoid a spurious delta spike when strafing resumes after a pause, exactly like the source plugin does). airborne/strafing gate whether the gauge itself updates this tick, strafing = exactly one of a/d held, matching strafetrainer.sp's `(moveleft != moveright)` check
void tickStrafeTrainer(StrafeTrainerState& state, float currentYawDeg, float horizontalSpeed, float maxSpeed,
                        float airSpeedCap, float dt, bool airborne, bool strafing);

// draws the "[----o--|----]" bar + percentage as an imgui overlay. call between ImGuiLayer::newFrame() and render(). no ops drawing nothing useful if airborne is false (strafetrainer.sp also only shows this in the air)
void drawStrafeTrainerBar(const StrafeTrainerState& state, bool airborne);

}  // namespace ui
