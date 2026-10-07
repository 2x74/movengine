#pragma once

#include <string>
#include <vector>

#include "collision/map_triggers.h"
#include "movement/player_move.h"

namespace movement {

struct TriggerRuntime {
    std::vector<bool> touching;   // passed the filter on entry and still overlapping
    std::vector<bool> disabled;   // StartDisabled (no I/O can enable it yet), or a spent trigger_once
    std::vector<float> nextFireTime;
    std::vector<std::vector<int>> outputFireCounts;
    std::string activatorName;    // the player's targetname as set by AddOutput, what name filters test
    float time = 0.0f;
    bool teleportedLastTick = false;
    // bhop blocks (mpbhops style): the block stood on, and its stand timer
    int standingBlock = -1;
    int lastBlock = -1;
    float punishTime = 0.0f;
    float health = 100.0f;  // only trigger_hurt touches it
};

struct TriggerTickResult {
    bool teleported = false;
    bool killed = false;   // a trigger_hurt took the last of your health
    bool setView = false;  // teleport without "preserve angles": snap the view to the destination's
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
};

void resetTriggerRuntime(TriggerRuntime& runtime, const collision::MapTriggers& triggers);

// run after tickPlayerMove against the post move position: start/end touch bookkeeping, then each touching trigger's behavior. teleports end the tick (everything else re-evaluates from the new position next tick). output delays are ignored, every map seen so far uses 0
// recorrectTeleportMomentum turns the speed you arrive with to face the way the destination does, instead of carrying the direction you went in with
TriggerTickResult tickTriggers(TriggerRuntime& runtime, const collision::MapTriggers& triggers, PlayerState& state,
                               const MoveConstants& constants, float dt,
                               bool recorrectTeleportMomentum = false);

}  // namespace movement
