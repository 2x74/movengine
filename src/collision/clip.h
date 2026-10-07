#pragma once

#include <glm/glm.hpp>

#include "collision/world_brushes.h"

namespace collision {

// pushes `center` out of the most penetrated brush side(s) it overlaps, treating the moving object as an aabb of the given half extents (minkowski sum against each brush's planes: expand the plane, test the box center as a point). returns true if any penetration was resolved, when true and outNormal is non null it's set to the last resolved exit normal (good enough for a simple "did we land on something roughly upward" check). outBrush, when non null, is set to the last brush that triggered a push, debug only (side count/shape of whatever's producing an unexpectedly large correction), not used by normal gameplay callers
bool resolveAabbVsBrushes(const WorldBrushes& world,
                           glm::vec3& center,
                           const glm::vec3& halfExtents,
                           glm::vec3* outNormal = nullptr,
                           const Brush** outBrush = nullptr);

struct TraceResult {
    float fraction = 1.0f;  // how far along start->end the box got before hitting something
    glm::vec3 endPos{0.0f};
    glm::vec3 normal{0.0f};  // surface hit, when fraction < 1
    bool startSolid = false;  // started inside a brush
    bool allSolid = false;    // never got out of it
};

// sweeps the box from start to end and stops it just short of the first brush in the way, like source's box traces (brush planes pushed out by the box's extents, then a ray through them). unlike resolveAabbVsBrushes this can't skip through a brush thinner than one tick's movement
TraceResult traceBox(const WorldBrushes& world, const glm::vec3& start, const glm::vec3& end,
                     const glm::vec3& halfExtents);

}  // namespace collision
