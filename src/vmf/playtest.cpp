#include "vmf/playtest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

#include "vmf/bsp_import.h"
#include "vmf/geometry.h"

namespace vmf {

namespace {

bool solidIsNonSolidTool(const Solid& solid) {
    return !solid.sides.empty() && std::all_of(solid.sides.begin(), solid.sides.end(), [](const Side& s) {
        return isNonSolidToolMaterial(s.material);
    });
}

collision::MapEntity toMapEntity(const Entity& e) {
    collision::MapEntity m;
    for (const auto& [k, v] : e.keyValues) m.keyValues.emplace_back(collision::toLowerAscii(k), v);
    for (const auto& [k, v] : e.connections) m.keyValues.emplace_back(collision::toLowerAscii(k), v);
    for (const auto& s : e.solids) m.brushes.push_back(solidBrush(s));
    return m;
}

void growZone(ZoneBox& zone, const Entity& e) {
    for (const auto& s : e.solids) {
        glm::vec3 mn, mx;
        if (!solidBounds(s, mn, mx)) continue;
        if (!zone.defined) {
            zone = {true, mn, mx};
        } else {
            zone.min = glm::min(zone.min, mn);
            zone.max = glm::max(zone.max, mx);
        }
    }
}

// "_light" / "_ambient": "r g b brightness" (0-255 color, ~200 typical).
glm::vec3 lightValue(const std::string& s, float brightnessScale) {
    std::istringstream iss(s);
    glm::vec4 v(255.0f, 255.0f, 255.0f, 200.0f);
    iss >> v.r >> v.g >> v.b >> v.a;
    glm::vec3 c = glm::vec3(v) / 255.0f * (v.a / brightnessScale);
    // the shader wants these in gamma space, 0 to 1, and raises them to the 2.2 on the way in, so anything over 1 doesn't merely clip, it explodes. the brightness divisors above assume a value near 200 but a real light_environment routinely carries 999 (bhop_icetrap does), which lands at about 5 and therefore 30 once linearised: the whole map rendered as a blown out wash with its colour pulled toward the sun's. cap the magnitude and keep the hue, which leaves every light already inside the range exactly as it was
    float peak = std::max({c.r, c.g, c.b});
    return peak > 1.0f ? c / peak : c;
}

}  // namespace

std::string zoneTargetname(int track, bool start) {
    if (track == 0) return start ? kStartZoneName : kEndZoneName;
    return "mod_zone_bonus_" + std::to_string(track) + (start ? "_start" : "_end");
}

bsp::SceneLighting sceneLighting(const Document& doc) {
    bsp::SceneLighting out;
    for (const auto& e : doc.entities) {
        std::string cls = collision::toLowerAscii(e.classname());
        if (cls == "light_environment") {
            out.hasLights = true;
            out.hasSun = true;
            // vrad's SetupLightNormalFromProps: yaw from angles, pitch from the "pitch" key when set, and pitch is the light's own elevation (negative points down), not a view pitch
            glm::vec3 angles = collision::parseKeyValueVec3(e.get("angles"));
            float pitch = collision::parseKeyValueFloat(e.get("pitch", "0"));
            if (pitch == 0.0f) pitch = angles.x;
            float yaw = glm::radians(angles.y), p = glm::radians(pitch);
            out.sunDirection = glm::normalize(glm::vec3(std::cos(p) * std::cos(yaw), std::cos(p) * std::sin(yaw), std::sin(p)));
            out.sunColor = lightValue(e.get("_light", "255 255 255 200"), 200.0f);
            out.ambient = lightValue(e.get("_ambient", "150 150 150 40"), 100.0f);
        } else if (cls == "light" || cls == "light_spot") {
            out.hasLights = true;
            bsp::PointLight l;
            l.position = collision::parseKeyValueVec3(e.get("origin"));
            l.color = lightValue(e.get("_light", "255 255 255 200"), 100.0f);
            l.fiftyPercentDistance = collision::parseKeyValueFloat(e.get("_fifty_percent_distance", "0"));
            if (l.fiftyPercentDistance <= 0.0f) l.fiftyPercentDistance = 128.0f;
            out.points.push_back(l);
        }
    }
    return out;
}

bsp::SceneFog sceneFog(const Document& doc) {
    bsp::SceneFog out;
    for (const auto& e : doc.entities) {
        if (collision::toLowerAscii(e.classname()) != "env_fog_controller") continue;
        // "fogenable" off means the map carries fog it does not use.
        if (collision::parseKeyValueInt(e.get("fogenable", "1")) == 0) continue;
        glm::vec3 c = collision::parseKeyValueVec3(e.get("fogcolor", "128 160 192"));
        out.color = glm::clamp(c / 255.0f, glm::vec3(0.0f), glm::vec3(1.0f));
        out.start = collision::parseKeyValueFloat(e.get("fogstart", "500"));
        out.end = collision::parseKeyValueFloat(e.get("fogend", "2000"));
        out.maxDensity = collision::parseKeyValueFloat(e.get("fogmaxdensity", "1"));
        // a controller with the two the wrong way round, or equal, would make the shader divide by nothing, give it a usable depth instead
        if (out.end <= out.start) out.end = out.start + 1.0f;
        out.maxDensity = glm::clamp(out.maxDensity, 0.0f, 1.0f);
        out.enabled = out.maxDensity > 0.0f;
        break;  // source uses the first one, so do we
    }
    return out;
}

PlaytestMap buildPlaytestMap(const Document& doc) {
    PlaytestMap out;
    out.pakfile = doc.world.get(kPakfileKey);
    MaterialTriangles triangles;

    for (const auto& s : doc.world.solids) {
        appendSolidTriangles(s, false, triangles, 1.0f);
        if (!solidIsNonSolidTool(s)) out.brushes.brushes.push_back(solidBrush(s));
    }

    std::vector<collision::MapEntity> mapEntities;
    for (const auto& e : doc.entities) {
        collision::MapEntity m = toMapEntity(e);
        if (!e.solids.empty()) {
            for (const auto& s : e.solids) appendSolidTriangles(s, false, triangles, 1.0f);
            if (collision::brushEntitySolidForPlayer(m)) {
                for (const auto& b : m.brushes) out.brushes.brushes.push_back(b);
            }
        }

        std::string cls = collision::toLowerAscii(e.classname());
        std::string name = collision::toLowerAscii(e.get("targetname"));
        if (cls == "trigger_multiple") {
            for (int t = 0; t < kZoneTracks; ++t) {
                if (name == zoneTargetname(t, true)) growZone(out.zones[t].start, e);
                if (name == zoneTargetname(t, false)) growZone(out.zones[t].end, e);
            }
        }

        static constexpr std::array<const char*, 3> kSpawnClasses = {
            "info_player_start", "info_player_terrorist", "info_player_counterterrorist"};
        if (!out.hasSpawn && std::find(kSpawnClasses.begin(), kSpawnClasses.end(), cls) != kSpawnClasses.end()) {
            out.hasSpawn = true;
            out.spawnFeet = collision::parseKeyValueVec3(e.get("origin"));
            out.spawnYawDeg = collision::parseKeyValueVec3(e.get("angles")).y;
        }

        mapEntities.push_back(std::move(m));
    }
    out.triggers = collision::buildMapTriggers(mapEntities);
    out.lighting = sceneLighting(doc);

    for (auto& [material, tris] : triangles) {
        out.geometry.groups.push_back({material, std::move(tris)});
    }
    return out;
}

}  // namespace vmf
