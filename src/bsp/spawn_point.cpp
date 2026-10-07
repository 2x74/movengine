#include "bsp/spawn_point.h"

#include <bsppp/BSP.h>

#include <array>
#include <cstdio>
#include <sstream>

namespace bsp {

namespace {

std::optional<glm::vec3> parseVec3(std::string_view s) {
    std::istringstream iss{std::string(s)};
    float x, y, z;
    if (iss >> x >> y >> z) {
        return glm::vec3(x, y, z);
    }
    return std::nullopt;
}

// source's "angles" keyvalue is "pitch yaw roll", in that order
std::optional<float> parseYawFromAngles(std::string_view s) {
    std::istringstream iss{std::string(s)};
    float pitch, yaw, roll;
    if (iss >> pitch >> yaw >> roll) {
        return yaw;
    }
    return std::nullopt;
}

}  // namespace

std::optional<SpawnPoint> loadSpawnPoint(const std::string& path) {
    bsppp::BSP file(path);
    if (!file) {
        std::fprintf(stderr, "failed to open bsp for spawn point: %s\n", path.c_str());
        return std::nullopt;
    }

    auto entities = file.getLumpData<bsppp::BSPLump::ENTITIES>();

    static constexpr std::array<const char*, 3> kSpawnClassnames = {
        "info_player_start", "info_player_terrorist", "info_player_counterterrorist",
    };

    for (const char* classname : kSpawnClassnames) {
        for (const auto& entity : entities) {
            if (!entity.hasChild("classname")) {
                continue;
            }
            if (entity("classname").getValue<std::string_view>() != classname) {
                continue;
            }
            if (!entity.hasChild("origin")) {
                continue;
            }
            auto pos = parseVec3(entity("origin").getValue<std::string_view>());
            if (!pos) {
                continue;
            }
            float yaw = 0.0f;
            if (entity.hasChild("angles")) {
                if (auto parsedYaw = parseYawFromAngles(entity("angles").getValue<std::string_view>())) {
                    yaw = *parsedYaw;
                }
            }
            std::printf("spawn point: %s at (%.1f, %.1f, %.1f) yaw=%.1f\n", classname, pos->x, pos->y, pos->z, yaw);
            return SpawnPoint{*pos, yaw};
        }
    }

    std::printf("no info_player_start/terrorist/counterterrorist found in %s\n", path.c_str());
    return std::nullopt;
}

}  // namespace bsp
