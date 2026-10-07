#pragma once

#include <glm/glm.hpp>
#include <vector>

namespace collision {

// outward facing half space plane: a point p is on the solid side when dot(normal, p) <= dist
struct Plane {
    glm::vec3 normal;
    float dist;
};

// a convex solid volume as the intersection of its sides' half spaces
struct Brush {
    std::vector<Plane> sides;
};

// all CONTENTS_SOLID ish brushes in the map, collected globally (no bsp tree traversal, no per model association, see bsp_brush_loader.cpp for why)
struct WorldBrushes {
    std::vector<Brush> brushes;
};

}  // namespace collision
