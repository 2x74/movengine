#include "collision/map_triggers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace collision {

std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

int parseKeyValueInt(const std::string& s, int fallback) {
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    return end == s.c_str() ? fallback : static_cast<int>(v);
}

float parseKeyValueFloat(const std::string& s, float fallback) {
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

glm::vec3 parseKeyValueVec3(const std::string& s) {
    std::istringstream iss(s);
    glm::vec3 v(0.0f);
    iss >> v.x >> v.y >> v.z;
    return iss ? v : glm::vec3(0.0f);
}

glm::mat3 sourceAngleMatrix(const glm::vec3& angles) {
    float sp = std::sin(glm::radians(angles.x)), cp = std::cos(glm::radians(angles.x));
    float sy = std::sin(glm::radians(angles.y)), cy = std::cos(glm::radians(angles.y));
    float sr = std::sin(glm::radians(angles.z)), cr = std::cos(glm::radians(angles.z));
    glm::vec3 forward(cp * cy, cp * sy, -sp);
    glm::vec3 left(sp * sr * cy - cr * sy, sp * sr * sy + cr * cy, sr * cp);
    glm::vec3 up(sp * cr * cy + sr * sy, sp * cr * sy - sr * cy, cr * cp);
    return glm::mat3(forward, left, up);
}

// everything else is a volume (triggers, rain, dust), purely visual (illusionary), or gameplay bookkeeping (buy zones, areaportals), none of which block players
bool brushEntitySolidForPlayer(const MapEntity& e) {
    std::string cls = toLowerAscii(e.value("classname"));
    if (cls.rfind("trigger_", 0) == 0) {
        return false;
    }
    static constexpr std::array<std::string_view, 17> kNonSolid = {
        "func_illusionary",    "func_precipitation", "func_dustmotes",     "func_dustcloud",
        "func_smokevolume",    "func_areaportal",    "func_areaportalwindow", "func_occluder",
        "func_buyzone",        "func_bomb_target",   "func_hostage_rescue", "func_clip_vphysics",
        "func_ladder",         "func_nobuild",       "func_no_defuse",     "func_vehicleclip",
        "func_fish_pool",
    };
    for (auto nonSolid : kNonSolid) {
        if (cls == nonSolid) return false;
    }
    if (cls == "func_brush") {
        // solidity: 0 = toggle (solid while enabled), 1 = never, 2 = always
        int solidity = parseKeyValueInt(e.value("solidity", "0"));
        if (solidity == 1) return false;
        if (solidity == 0 && parseKeyValueInt(e.value("startdisabled", "0")) != 0) return false;
    }
    if (cls == "func_wall_toggle" && (parseKeyValueInt(e.value("spawnflags", "0")) & 1) != 0) {
        return false;  // "starts invisible"
    }
    return true;
}

namespace {

// splits "target,input,param,delay,times", older compilers separate the fields with ',', newer ones with ESC (0x1B)
std::optional<EntityOutput> parseOutput(const std::string& event, const std::string& raw) {
    char sep = raw.find('\x1b') != std::string::npos ? '\x1b' : ',';
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        size_t pos = raw.find(sep, start);
        fields.push_back(raw.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    if (fields.size() < 2) {
        return std::nullopt;
    }
    EntityOutput out;
    out.event = event;
    out.target = fields[0];
    out.input = fields[1];
    out.param = fields.size() > 2 ? fields[2] : "";
    out.timesToFire = fields.size() > 4 ? parseKeyValueInt(fields[4], -1) : -1;
    return out;
}

}  // namespace

MapTriggers buildMapTriggers(const std::vector<MapEntity>& entities) {
    // source's name lookups are case insensitive (surf_kitsune's teleports target "Orange" at a destination named "orange"), first match wins
    std::unordered_map<std::string, const MapEntity*> entityByName;
    for (const auto& e : entities) {
        std::string name = toLowerAscii(e.value("targetname"));
        if (!name.empty()) {
            entityByName.emplace(name, &e);
        }
    }

    MapTriggers out;
    int counts[5] = {0, 0, 0, 0, 0};
    for (const auto& e : entities) {
        std::string cls = toLowerAscii(e.value("classname"));
        Trigger t;
        if (cls == "trigger_teleport") t.kind = TriggerKind::Teleport;
        else if (cls == "trigger_push") t.kind = TriggerKind::Push;
        else if (cls == "trigger_multiple") t.kind = TriggerKind::Multiple;
        else if (cls == "trigger_once") t.kind = TriggerKind::Once;
        else if (cls == "trigger_hurt") t.kind = TriggerKind::Hurt;
        else continue;

        t.spawnflags = parseKeyValueInt(e.value("spawnflags", "0"));
        // triggers ignore players unless "Clients" (1) or "Everything" (64) is set
        if ((t.spawnflags & (1 | 64)) == 0 || e.brushes.empty()) {
            continue;
        }
        t.brushes = e.brushes;
        t.startDisabled = parseKeyValueInt(e.value("startdisabled", "0")) != 0;

        std::string filterName = toLowerAscii(e.value("filtername"));
        if (!filterName.empty()) {
            auto filterIt = entityByName.find(filterName);
            if (filterIt != entityByName.end() &&
                toLowerAscii(filterIt->second->value("classname")) == "filter_activator_name") {
                t.filter = ActivatorNameFilter{filterIt->second->value("filtername"),
                                               parseKeyValueInt(filterIt->second->value("negated", "0")) != 0};
            }
        }

        if (t.kind == TriggerKind::Multiple) {
            // CTriggerMultiple::Spawn turns an unset wait into 0.2s
            float wait = parseKeyValueFloat(e.value("wait", "0"));
            t.wait = (wait == 0.0f) ? 0.2f : wait;
        }

        if (t.kind == TriggerKind::Teleport) {
            auto destIt = entityByName.find(toLowerAscii(e.value("target")));
            if (destIt != entityByName.end()) {
                t.hasDestination = true;
                t.destinationOrigin = parseKeyValueVec3(destIt->second->value("origin"));
                t.destinationAngles = parseKeyValueVec3(destIt->second->value("angles"));
            } else {
                std::printf("trigger_teleport: target \"%s\" not found, ignoring\n", e.value("target").c_str());
            }
        }

        if (t.kind == TriggerKind::Hurt) {
            t.damage = parseKeyValueFloat(e.value("damage", "10"));
        }

        if (t.kind == TriggerKind::Push) {
            // pushdir is world-space angles: CTriggerPush::Spawn converts it
            // into entity space and Touch rotates it back, which cancels out.
            t.pushDir = sourceAngleMatrix(parseKeyValueVec3(e.value("pushdir")))[0];
            t.pushSpeed = parseKeyValueFloat(e.value("speed", "40"));
        }

        for (const auto& [key, value] : e.keyValues) {
            if (key.rfind("on", 0) == 0) {
                if (auto output = parseOutput(key, value)) {
                    t.outputs.push_back(std::move(*output));
                }
            }
        }

        counts[static_cast<int>(t.kind)]++;
        out.triggers.push_back(std::move(t));
    }

    // bhop blocks (mpbhops_but_working): touch activated doors/buttons that move down onto a teleport, or up and back (boosters)
    constexpr int kDoorPlayerTouch = 1024, kButtonTouchActivates = 256;
    int teleBlocks = 0, boostBlocks = 0;
    for (const auto& e : entities) {
        std::string cls = toLowerAscii(e.value("classname"));
        int flags = parseKeyValueInt(e.value("spawnflags", "0"));
        bool door = cls == "func_door" && (flags & kDoorPlayerTouch);
        bool button = cls == "func_button" && (flags & kButtonTouchActivates);
        if ((!door && !button) || e.brushes.empty()) continue;

        glm::vec3 mn(1e9f), mx(-1e9f);
        for (const auto& b : e.brushes) {
            glm::vec3 bmn, bmx;
            if (brushBounds(b, bmn, bmx)) {
                mn = glm::min(mn, bmn);
                mx = glm::max(mx, bmx);
            }
        }
        if (mn.x > mx.x) continue;
        // CBaseDoor/CBaseButton: travel along movedir by the brush's extent in that direction, minus the lip
        glm::vec3 dir = sourceAngleMatrix(parseKeyValueVec3(e.value("movedir")))[0];
        float travel = std::abs(glm::dot(dir, mx - mn)) - parseKeyValueFloat(e.value("lip", "0"));
        glm::vec3 offset = dir * travel;

        BhopBlock block;
        block.brushes = e.brushes;
        if (offset.z < -0.1f) {
            // whatever teleport it would sink into
            glm::vec3 sweepMin = glm::min(mn, mn + offset), sweepMax = glm::max(mx, mx + offset);
            for (int index = 0; index < static_cast<int>(out.triggers.size()); ++index) {
                if (out.triggers[index].kind != TriggerKind::Teleport) continue;
                for (const auto& tb : out.triggers[index].brushes) {
                    glm::vec3 tmn, tmx;
                    if (brushBounds(tb, tmn, tmx) && glm::all(glm::lessThanEqual(tmn, sweepMax)) &&
                        glm::all(glm::greaterThanEqual(tmx, sweepMin))) {
                        block.teleportTrigger = index;
                        break;
                    }
                }
                if (block.teleportTrigger >= 0) break;
            }
        } else if (offset.z > 0.1f && parseKeyValueFloat(e.value("wait", "0")) > 0.0f) {
            block.boostSpeed = parseKeyValueFloat(e.value("speed", "100"));
        }
        if (block.teleportTrigger < 0 && block.boostSpeed <= 0.0f) continue;
        (block.teleportTrigger >= 0 ? teleBlocks : boostBlocks)++;
        out.bhopBlocks.push_back(std::move(block));
    }

    std::printf("triggers: %d teleport, %d push, %d multiple, %d once, %d hurt; bhop blocks: %d teleport, %d booster\n",
                counts[0], counts[1], counts[2], counts[3], counts[4], teleBlocks, boostBlocks);
    return out;
}

bool brushBounds(const Brush& brush, glm::vec3& min, glm::vec3& max) {
    glm::vec3 lo(-1e30f), hi(1e30f);
    for (const auto& s : brush.sides) {
        for (int a = 0; a < 3; ++a) {
            // exactly axial only: an edge bevel can be within a hair of an axis and sit well outside the bounds
            if (s.normal[a] > 0.99999f) hi[a] = std::min(hi[a], s.dist);
            if (s.normal[a] < -0.99999f) lo[a] = std::max(lo[a], -s.dist);
        }
    }
    if (lo.x <= -1e29f || lo.y <= -1e29f || lo.z <= -1e29f || hi.x >= 1e29f || hi.y >= 1e29f || hi.z >= 1e29f) {
        return false;
    }
    min = lo;
    max = hi;
    return true;
}

}  // namespace collision
