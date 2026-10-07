#pragma once

#include <glm/glm.hpp>

#include <array>
#include <string>

#include "bsp/world_geometry.h"
#include "collision/map_triggers.h"
#include "collision/world_brushes.h"
#include "vmf/document.h"

namespace vmf {

// zones authored in the map as trigger_multiple with targetnames in the "mod_zone" convention shavit's prebuilt zone support reads, so a compiled map gets its start/end zones on a real server too: mod_zone_start / mod_zone_end for the main course, mod_zone_bonus_<n>_start / _end for bonus n
inline constexpr const char* kStartZoneName = "mod_zone_start";
inline constexpr const char* kEndZoneName = "mod_zone_end";
inline constexpr int kZoneTracks = 9;  // main + 8 bonuses, matches movement::kMaxTracks

std::string zoneTargetname(int track, bool start);

struct ZoneBox {
    bool defined = false;
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

struct TrackZoneBoxes {
    ZoneBox start;
    ZoneBox end;
};

// everything the game needs to play a .vmf directly, without compiling
struct PlaytestMap {
    // visible faces (tool textures hidden, like in game). uvs are in texels: divide by each material's texture size once it's resolved
    bsp::WorldGeometry geometry;
    collision::WorldBrushes brushes;
    collision::MapTriggers triggers;
    bool hasSpawn = false;
    glm::vec3 spawnFeet{0.0f};
    float spawnYawDeg = 0.0f;
    std::array<TrackZoneBoxes, kZoneTracks> zones;
    bsp::SceneLighting lighting;  // live preview of the map's light entities
    std::string pakfile;  // a decompiled map's source .bsp, whose packed textures it uses
};

PlaytestMap buildPlaytestMap(const Document& doc);

// light / light_spot / light_environment entities as live lights. The sun
// uses vrad's own direction math (pitch -90 = straight down).
bsp::SceneLighting sceneLighting(const Document& doc);

// the map's env_fog_controller, if it has one that's switched on. source writes fog colours as "r g b" in 0-255
bsp::SceneFog sceneFog(const Document& doc);

}  // namespace vmf
