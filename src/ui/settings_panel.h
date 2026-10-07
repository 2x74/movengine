#pragma once

#include "console/cvar.h"

namespace ui {

// draws the "edit > settings..." window if open. every cvar in the registry gets a slider, changing one calls CvarRegistry::set() so onChange fires the same as it would from a cfg exec
void drawSettingsPanel(console::CvarRegistry& cvars, bool& open);

}  // namespace ui
