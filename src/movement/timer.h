#pragma once

#include <glm/glm.hpp>

#include "movement/zones.h"

namespace movement {

struct TimerState {
    int track = 0;  // which course the current/last run is on (0 = main, see zones.h)
    bool running = false;
    bool wasInStartZone = false;
    float elapsedSeconds = 0.0f;
    int jumps = 0;
    int airTicks = 0;      // sync stat denominator (approximate, see tickTimer)
    int goodAirTicks = 0;  // sync stat numerator: ticks where air-strafing actually gained speed
    float lastRunTimeSeconds = -1.0f;
    // this tick only: the run started (left the start zone) / finished (lastRunTimeSeconds is its time). pbs are kept by the caller (see personal_bests.h) per style
    bool justStarted = false;
    bool justFinished = false;
};

// stops and clears the run, including the last finished time
void resetTimerRun(TimerState& timer);

// simplified stand in for shavit's real zone/prespeed rules (not a faithful port like the movement styles): entering any track's start zone arms that track, the timer starts the tick you leave it and stops on entering that track's end zone. sync is approximated as the fraction of airborne ticks where air accelerate actually added speed, not shavit's real per strafe key formula
void tickTimer(TimerState& timer, const MapZones& zones, const glm::vec3& playerPos,
               const glm::vec3& playerHalfExtents, bool grounded, bool justJumped, bool speedGainedInAir, float dt);

// point the timer at another track (restarting onto it, or arming it)
void switchTimerTrack(TimerState& timer, int track);

}  // namespace movement
