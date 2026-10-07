#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "collision/world_brushes.h"

namespace collision {

enum class TriggerKind { Teleport, Push, Multiple, Once, Hurt };

// one parsed "OnEndTouch" style entity i/o connection. only the !activator AddOutput forms bhop/surf maps actually rely on get executed at runtime (basevelocity boosters, targetname tagging for filters), everything else is parsed and ignored
struct EntityOutput {
    std::string event;   // lowercased: onstarttouch / onendtouch / ontrigger
    std::string target;
    std::string input;
    std::string param;
    int timesToFire = -1;  // -1 = unlimited
};

// filter_activator_name: passes when the activator's targetname matches
// (case-insensitive), inverted when negated.
struct ActivatorNameFilter {
    std::string matchName;
    bool negated = false;
};

struct Trigger {
    TriggerKind kind = TriggerKind::Multiple;
    std::vector<Brush> brushes;  // already moved/rotated into world space
    bool startDisabled = false;
    int spawnflags = 0;
    // unset when the trigger has no filter, or names one we can't evaluate (missing, or a non name filter class), source passes those too
    std::optional<ActivatorNameFilter> filter;
    float wait = 0.2f;  // trigger_multiple refire delay; <= 0 means fire once

    bool hasDestination = false;  // trigger_teleport
    glm::vec3 destinationOrigin{0.0f};  // feet-level, like Source's entity origins
    glm::vec3 destinationAngles{0.0f};  // pitch yaw roll

    glm::vec3 pushDir{0.0f};  // trigger_push, world-space unit vector
    float pushSpeed = 0.0f;

    float damage = 0.0f;  // trigger_hurt, per second (100+ kills straight away)

    std::vector<EntityOutput> outputs;
};

// an old school bhop block: a func_door / func_button that moves when you touch it. like mpbhops_but_working it stays put instead, and:
struct BhopBlock {
    std::vector<Brush> brushes;
    // ...a block that would sink into a trigger_teleport sends you through
    // that teleport when you stand on it too long (index into triggers),
    int teleportTrigger = -1;
    // ...and one that would rise launches you this fast when you jump off it.
    float boostSpeed = 0.0f;
};

struct MapTriggers {
    std::vector<Trigger> triggers;
    std::vector<BhopBlock> bhopBlocks;
};

// aabb of a brush from its axial (bevel) planes, false if it has none
bool brushBounds(const Brush& brush, glm::vec3& min, glm::vec3& max);

// a map entity in loader neutral form so .bsp and .vmf maps share one set of entity rules. keys are lowercased (source treats them case insensitively) and kept in order since repeated output keys like OnEndTouch must all survive. brushes are already in world space
struct MapEntity {
    std::vector<std::pair<std::string, std::string>> keyValues;
    std::vector<Brush> brushes;

    std::string value(std::string_view lowerKey, const std::string& fallback = "") const {
        for (const auto& [k, v] : keyValues) {
            if (k == lowerKey) return v;
        }
        return fallback;
    }
};

// whether a brush entity blocks players in game (func_detail/func_brush do, triggers, func_illusionary, rain volumes etc don't)
bool brushEntitySolidForPlayer(const MapEntity& entity);

// trigger_teleport / trigger_push / trigger_multiple / trigger_once, with
// destinations, filters and outputs resolved against the other entities.
MapTriggers buildMapTriggers(const std::vector<MapEntity>& entities);

// keyvalue parsing helpers shared by the loaders. non throwing, mapper typed values aren't guaranteed to be well formed
int parseKeyValueInt(const std::string& s, int fallback = 0);
float parseKeyValueFloat(const std::string& s, float fallback = 0.0f);
glm::vec3 parseKeyValueVec3(const std::string& s);
// source's AngleMatrix: (pitch, yaw, roll) degrees -> rotation whose columns are forward/left/up in world space
glm::mat3 sourceAngleMatrix(const glm::vec3& anglesDeg);
std::string toLowerAscii(std::string_view s);

// conservative aabb vs convex brush overlap: separated only if some brush plane has the whole box on its outside. brushes carry their bevel planes so this matches what source's box vs brush touch test sees
inline bool aabbOverlapsBrush(const Brush& brush, const glm::vec3& center, const glm::vec3& halfExtents) {
    for (const auto& side : brush.sides) {
        float reach = std::abs(side.normal.x) * halfExtents.x + std::abs(side.normal.y) * halfExtents.y +
                      std::abs(side.normal.z) * halfExtents.z;
        if (glm::dot(side.normal, center) - reach > side.dist) {
            return false;
        }
    }
    return !brush.sides.empty();
}

}  // namespace collision
