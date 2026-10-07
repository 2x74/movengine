#include "collision/clip.h"

#include <algorithm>
#include <limits>

namespace collision {

namespace {

// if the box (as a minkowski expanded point) is inside this brush, returns true with the least penetration exit side. otherwise returns false
bool testBrush(const Brush& brush, const glm::vec3& center, const glm::vec3& halfExtents,
                float& outPenetration, glm::vec3& outNormal) {
    float minPenetration = std::numeric_limits<float>::max();
    glm::vec3 minNormal(0.0f);

    for (const auto& side : brush.sides) {
        float expandedDist = side.dist + glm::dot(glm::abs(side.normal), halfExtents);
        float penetration = expandedDist - glm::dot(side.normal, center);
        if (penetration < 0.0f) {
            // outside this side's half space, so outside the (convex) brush
            return false;
        }
        if (penetration < minPenetration) {
            minPenetration = penetration;
            minNormal = side.normal;
        }
    }

    outPenetration = minPenetration;
    outNormal = minNormal;
    return true;
}

}  // namespace

bool resolveAabbVsBrushes(const WorldBrushes& world,
                           glm::vec3& center,
                           const glm::vec3& halfExtents,
                           glm::vec3* outNormal,
                           const Brush** outBrush) {
    bool resolvedAny = false;

    // a few passes so pushing out of one brush doesn't leave the box embedded in a neighbor at a corner/seam
    for (int pass = 0; pass < 4; ++pass) {
        bool resolvedThisPass = false;
        for (const auto& brush : world.brushes) {
            float penetration;
            glm::vec3 normal;
            if (testBrush(brush, center, halfExtents, penetration, normal)) {
                center += normal * (penetration + 0.01f);
                if (outNormal) {
                    *outNormal = normal;
                }
                if (outBrush) {
                    *outBrush = &brush;
                }
                resolvedAny = true;
                resolvedThisPass = true;
            }
        }
        if (!resolvedThisPass) {
            break;
        }
    }

    return resolvedAny;
}

TraceResult traceBox(const WorldBrushes& world, const glm::vec3& start, const glm::vec3& end,
                     const glm::vec3& halfExtents) {
    // stay this far off surfaces so the next trace doesn't start touching them
    constexpr float kDistEpsilon = 0.03125f;

    TraceResult trace;
    for (const auto& brush : world.brushes) {
        float enterFrac = -1.0f;
        float leaveFrac = 1.0f;
        glm::vec3 clipNormal(0.0f);
        bool startOut = false, getOut = false, missed = false;
        for (const auto& side : brush.sides) {
            float dist = side.dist + glm::dot(glm::abs(side.normal), halfExtents);
            float d1 = glm::dot(start, side.normal) - dist;
            float d2 = glm::dot(end, side.normal) - dist;
            if (d2 > 0.0f) getOut = true;
            if (d1 > 0.0f) startOut = true;
            // entirely in front of this face: can't touch the brush at all. quake/source also count ending within kDistEpsilon of the face as touching, dropped because gliding along a surf ramp that hugs the face then "enters" the next coplanar ramp brush through its vertical seam face and dead stops you (rampbug)
            if (d1 > 0.0f && d2 >= 0.0f) {
                missed = true;
                break;
            }
            if (d1 <= 0.0f && d2 <= 0.0f) continue;
            if (d1 > d2) {
                float f = std::max(0.0f, (d1 - kDistEpsilon) / (d1 - d2));
                if (f > enterFrac) {
                    enterFrac = f;
                    clipNormal = side.normal;
                }
            } else {
                float f = std::min(1.0f, (d1 + kDistEpsilon) / (d1 - d2));
                leaveFrac = std::min(leaveFrac, f);
            }
        }
        if (missed || brush.sides.empty()) continue;
        if (!startOut) {
            trace.startSolid = true;
            if (!getOut) {
                trace.allSolid = true;
                trace.fraction = 0.0f;
            }
            continue;
        }
        if (enterFrac < leaveFrac && enterFrac > -1.0f && enterFrac < trace.fraction) {
            trace.fraction = std::max(0.0f, enterFrac);
            trace.normal = clipNormal;
        }
    }
    trace.endPos = start + (end - start) * trace.fraction;
    return trace;
}

}  // namespace collision
