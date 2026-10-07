#include "movement/timer.h"

namespace movement {

void resetTimerRun(TimerState& timer) {
    timer.running = false;
    timer.elapsedSeconds = 0.0f;
    timer.jumps = 0;
    timer.airTicks = 0;
    timer.goodAirTicks = 0;
    timer.lastRunTimeSeconds = -1.0f;  // a restart / new attempt clears the last time off the HUD
}

void switchTimerTrack(TimerState& timer, int track) {
    if (track == timer.track) return;
    resetTimerRun(timer);
    timer.track = track;
    timer.lastRunTimeSeconds = -1.0f;
}

void tickTimer(TimerState& timer, const MapZones& zones, const glm::vec3& playerPos,
               const glm::vec3& playerHalfExtents, bool grounded, bool justJumped, bool speedGainedInAir, float dt) {
    timer.justStarted = false;
    timer.justFinished = false;
    int inStartOf = -1;
    for (int t = 0; t < kMaxTracks && inStartOf < 0; ++t) {
        if (hullInZone(zones.tracks[t].start, playerPos, playerHalfExtents)) inStartOf = t;
    }

    if (inStartOf >= 0) {
        if (!timer.wasInStartZone || timer.track != inStartOf) {
            switchTimerTrack(timer, inStartOf);
            resetTimerRun(timer);
        }
        timer.wasInStartZone = true;
    } else {
        if (timer.wasInStartZone) {
            timer.running = true;
            timer.justStarted = true;
        }
        timer.wasInStartZone = false;
    }

    if (timer.running) {
        timer.elapsedSeconds += dt;
        if (justJumped) {
            timer.jumps++;
        }
        if (!grounded) {
            timer.airTicks++;
            if (speedGainedInAir) {
                timer.goodAirTicks++;
            }
        }

        if (hullInZone(zones.tracks[timer.track].end, playerPos, playerHalfExtents)) {
            timer.running = false;
            timer.lastRunTimeSeconds = timer.elapsedSeconds;
            timer.justFinished = true;
        }
    }
}

}  // namespace movement
