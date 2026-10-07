#pragma once

#include <string>

namespace platform {

// where this app keeps settings, personal bests, keybinds and window layouts. created on first use, and ends with a separator
//
// the app used to be called bhop-localhops so the first run under the new name carries across whatever the old folder held instead of starting everyone from nothing
const std::string& prefDir();

// a file inside prefDir()
std::string prefFile(const std::string& name);

}  // namespace platform
