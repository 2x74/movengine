#pragma once

#include <optional>
#include <string>

#include "collision/map_triggers.h"
#include "collision/world_brushes.h"

namespace collision {

std::optional<WorldBrushes> loadWorldBrushes(const std::string& path);

// trigger_teleport / trigger_push / trigger_multiple / trigger_once volumes,
// with their teleport destinations, filters and outputs resolved up front.
std::optional<MapTriggers> loadMapTriggers(const std::string& path);

// every brush as compiled, for tools (the editor's .bsp import). planes are in the owning model's local space (entity models are relative to the entity's origin/angles), model is -1 for brushes no model reaches
struct BspBrushSide {
    Plane plane;
    int texInfo = -1;   // -1: no texture (nodraw)
    int dispInfo = -1;  // >= 0: the base face of a displacement
    bool bevel = false; // added by the compiler for collision, not a real face
};
struct BspBrush {
    int contents = 0;
    int model = -1;
    std::vector<BspBrushSide> sides;
};
std::optional<std::vector<BspBrush>> loadBspBrushes(const std::string& path);

}  // namespace collision
