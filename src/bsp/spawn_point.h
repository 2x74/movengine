#pragma once

#include <glm/glm.hpp>
#include <optional>
#include <string>

namespace bsp {

struct SpawnPoint {
    glm::vec3 position;
    float yawDeg;
};

// looks for info_player_start first, then falls back to the cs team spawns (info_player_terrorist/info_player_counterterrorist) since plenty of real maps, stock css ones included, only define those. returns nullopt if none exist so the caller can fall back to something hardcoded
std::optional<SpawnPoint> loadSpawnPoint(const std::string& path);

}  // namespace bsp
