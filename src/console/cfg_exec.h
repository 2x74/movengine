#pragma once

#include <string>

#include "console/cvar.h"

namespace console {

// parses and runs a source style .cfg file against the given registry: recognized `cvarname value` lines call set(), `exec otherfile.cfg` recurses (relative to the including file's directory first). everything else (binds, unrecognized commands/cvars) is logged and skipped instead of treated as an error, real css configs have plenty of commands this engine has no equivalent for yet, same as a real engine ignoring cvars from mods/plugins it doesn't have loaded
void execConfigFile(const std::string& path, CvarRegistry& cvars);

// writes the cvars changed from their defaults as `name value` lines (a plain .cfg that execConfigFile reads back) under kSettingsHeader, so changing a default reaches everyone who never touched that setting
bool saveConfigFile(const std::string& path, const CvarRegistry& cvars);
extern const char* const kSettingsHeader;

}  // namespace console
