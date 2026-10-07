#pragma once

#include <glm/glm.hpp>

#include <array>
#include <string>

namespace movement {

// shavit's default zone height (shavit_zones_height): corners are placed at
// floor level and the box extends this far up.
constexpr float kZoneHeight = 128.0f;

// axis aligned box zone, defined by two corners (min auto sorted from max). simplified version of the real tool (screenshot from angelgirl.cloud's shavit zone menu): walk to a corner, set it, walk to the opposite corner, set it. no grid snap/wall snap options yet, just the core two point box
struct Zone {
    bool defined = false;
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    // feet position of the first corner the player actually stood at, not the box center. guaranteed to be a real standable position (they were there) unlike the geometric center of an irregular/large box, which can land inside a wall if the zone spans uneven floor or a non convex room
    glm::vec3 anchorPoint{0.0f};
    // deleted on purpose: keeps a zone the map itself authors (a .vmf's mod_zone_* entities) from coming back on the next load
    bool removed = false;
};

// hull overlap like shavit's touch based zones. testing only the hull center against a zone is what made jumping or crouching in one flicker you in and out of it
inline bool hullInZone(const Zone& zone, const glm::vec3& center, const glm::vec3& halfExtents) {
    if (!zone.defined) return false;
    glm::vec3 lo = center - halfExtents;
    glm::vec3 hi = center + halfExtents;
    return lo.x <= zone.max.x && hi.x >= zone.min.x && lo.y <= zone.max.y && hi.y >= zone.min.y &&
           lo.z <= zone.max.z && hi.z >= zone.min.z;
}

// feetPos is where the player is standing (floor level), not their hull center.
inline void setZoneCorner(Zone& zone, bool& hasFirstCorner, glm::vec3& firstCorner, const glm::vec3& feetPos) {
    if (!hasFirstCorner) {
        firstCorner = feetPos;
        hasFirstCorner = true;
        return;
    }
    zone.min = glm::min(firstCorner, feetPos);
    zone.max = glm::max(firstCorner, feetPos);
    zone.max.z += kZoneHeight;
    zone.anchorPoint = firstCorner;
    zone.defined = true;
    zone.removed = false;
    hasFirstCorner = false;
}

// shavit's !setstart: a spot inside the start zone that restart sends you
// to (with your view) instead of the zone's middle.
struct StartPoint {
    bool defined = false;
    glm::vec3 feetPos{0.0f};
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
};

// track 0 is the main course, 1..8 are bonuses, each with its own zones and set start spot (shavit's !b <n>)
constexpr int kMaxTracks = 9;

struct TrackZones {
    Zone start;
    Zone end;
    StartPoint startPoint;
    bool used() const { return start.defined || end.defined; }
};

struct MapZones {
    std::array<TrackZones, kMaxTracks> tracks;
};

// shavit-style delete: the zone goes, and a start zone takes its track's
// set-start spot with it.
inline void removeZone(TrackZones& track, bool start) {
    Zone& zone = start ? track.start : track.end;
    zone = Zone{};
    zone.removed = true;
    if (start) track.startPoint = StartPoint{};
}

// "main", "bonus 1", ...
std::string trackName(int track);

// per map persistence, one line per defined zone: "start|end minx miny minz maxx maxy maxz ax ay az" ("start|end none" for a deleted one) and "setstart x y z yaw pitch" for the main track, same prefixed "bonus<n>_" for bonuses. a missing file just means nothing's been set on this map yet
void loadZones(const std::string& path, MapZones& zones);
bool saveZones(const std::string& path, const MapZones& zones);

}  // namespace movement
