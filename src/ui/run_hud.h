#pragma once

#include <string>

#include "movement/timer.h"

namespace ui {

// approximation of jhud's bottom center run display (no source was available for jhud, only a disabled compiled binary, laid out to match the screenshot's style/time/jumps/sync/speed block but it's a recreation from a screenshot, not a port). deliberately doesn't show sr/pb from leaderboard/os fields like the real jhud does since we have no server/leaderboard to pull those from, only your own saved pb
// bestSeconds: pb for this track + style (-1 = none), lastDelta: the last finish against the pb it was up against (NaN = nothing to compare)
void drawRunHud(const movement::TimerState& timer, const std::string& styleName, float horizontalSpeed,
                float bestSeconds, float lastDelta);

// "12.34", "1:02.34", "1:00:02.34"
std::string formatRunTime(float seconds);

}  // namespace ui
