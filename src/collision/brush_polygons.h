#pragma once

#include <glm/glm.hpp>
#include <vector>

#include "collision/world_brushes.h"

namespace collision {

// a convex brush's faces, one polygon per side that actually bounds it, wound ccw seen from outside. sides the other planes cut away entirely (a bevel that touches nothing) produce nothing
//
// brushes are stored only as half spaces (no edges to read back) so each face is recovered by starting with a huge quad on its plane and clipping it against every other plane of the brush. that's how vbsp builds brush windings too
struct BrushFace {
    // the side's own outward plane normal. carried instead of recomputed from the points: three consecutive points of a face can be very nearly collinear and the cross product then flips sign on rounding alone
    glm::vec3 normal{0.0f};
    std::vector<glm::vec3> points;
};

std::vector<BrushFace> brushPolygons(const Brush& brush);

}  // namespace collision
