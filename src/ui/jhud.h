#pragma once

namespace ui {

// per jump hud, the bhop server "jhud": jump number and takeoff speed each jump, and from the 2nd jump on the previous airtime's gain (speed added vs the most perfect strafing could have added) and sync
struct JhudState {
    int jumps = 0;
    float takeoffSpeed = 0.0f;
    bool hasStats = false;
    float gainPct = 0.0f;
    float syncPct = 0.0f;
    float sinceLastJump = 999.0f;
    float groundedTime = 0.0f;

    // accumulated over the current airtime
    float gainSum = 0.0f;
    float gainMaxSum = 0.0f;
    int airTicks = 0;
    int syncedTicks = 0;
};

void resetJhud(JhudState& state);

void tickJhud(JhudState& state, bool justJumped, bool grounded, float horizontalSpeed, float airGain,
              float airGainMax, bool speedGainedInAir, float dt);

// centered just under the crosshair, fades out ~2s after the last jump
void drawJhud(const JhudState& state);

}  // namespace ui
