#include "movement/styles.h"

namespace movement {

const std::vector<StyleDef>& styleList() {
    static const std::vector<StyleDef> kStyles = {
        // normal (default), shavit-styles.cfg style 0 "Normal"
        {"normal", false, false, false, false, false, 0, 1000.0f, 1.0f, true, false, false},
        // sideways: shavit style 1 "Sideways", blocks a+d and leaves w/s. confirmed from the real config, opposite of generic bhop community "sideways" naming but matches what's live
        {"sideways", false, true, false, true, false, 0, -1.0f, 1.0f, true, false, false},
        // half-sideways, style 6, force_hsw=1
        {"half-sideways", false, false, false, false, false, 1, -1.0f, 1.0f, true, false, false},
        // surf half-sideways, force_hsw=2 (style 0 comment says "2 for surf-HSW")
        {"surf half-sideways", false, false, false, false, false, 2, -1.0f, 1.0f, true, false, false},
        // legit (scroll), style 3 "Scroll": no auto bhop, much lower air accel
        {"legit (scroll)", false, false, false, false, false, 0, 100.0f, 1.0f, false, false, false},
        // lowgrav, style 9 "Low Gravity"
        {"lowgrav", false, false, false, false, false, 0, -1.0f, 0.5f, true, false, false},
        // a-only, style 7 "A/D-Only": blocks w+s, locks to first a/d pressed
        {"a-only", true, false, true, false, true, 0, 1000.0f, 1.0f, true, false, false},
        // infinite, style 12: midair jump + start zone reset (infinite.sp)
        {"infinite", false, false, false, false, false, 0, 1000.0f, 1.0f, true, true, false},
        // test2, not a numbered shavit style, auto strafe from test2dll.sp
        {"test2", false, false, false, false, false, 0, -1.0f, 1.0f, true, false, true},
    };
    return kStyles;
}

}  // namespace movement
