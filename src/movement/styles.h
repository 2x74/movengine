#pragma once

#include <string>
#include <vector>

namespace movement {

// mechanics verified against two sources on angelgirl.cloud, not guessed: shavit-core.sp (the real block_w/a/s/d, a_or_d_only and force_hsw 1/2 logic, ported faithfully below) and cfg/sourcemod/shavit-styles.cfg (per style values: airaccelerate, gravity etc). infinite's midair jump/start zone reset came from infinite.sp, test2's auto strafe from test2dll.sp
struct StyleDef {
    std::string name;
    bool blockW = false;
    bool blockA = false;
    bool blockS = false;
    bool blockD = false;
    bool aOrDOnly = false;   // lock to whichever of a/d is pressed first (not "only a", shavit-core locks to the first choice and doesn't hardcode a single key)
    int forceHsw = 0;        // 0 off, 1 = W+strafe combo required ("holding w while doing a/d"), 2 = surf steering lock (first W+A/S+D-or-W+D/S+A combo wins, for the whole run)
    float airAccelerateOverride = -1.0f;  // -1 = use whatever sv_airaccelerate is set to
    float gravityMultiplier = 1.0f;
    bool autoBhop = true;    // false = must release+repress jump each hop (legit/scroll)
    bool infiniteMidairJump = false;
    bool test2AutoStrafe = false;
};

// index matches the styles menu order in ui::MenuBarState::styleNames
const std::vector<StyleDef>& styleList();

}  // namespace movement
