#pragma once

#include <glm/glm.hpp>

#include <map>
#include <string>
#include <vector>

#include "bsp/world_geometry.h"
#include "collision/world_brushes.h"
#include "vmf/document.h"

namespace vmf {

struct Plane {
    glm::vec3 normal;  // outward
    float dist;        // inside when dot(normal, p) <= dist
};

// vbsp's PlaneFromPoints: normal = (p0 - p1) x (p2 - p1).
Plane sidePlane(const Side& side);

// one plane per side, outward. a solid wound the other way round (some third party writers) is detected (it has no interior) and flipped
std::vector<Plane> solidPlanes(const Solid& solid);

// one convex polygon per side, in side order, wound ccw seen from outside, empty for a side that doesn't touch the solid
std::vector<std::vector<glm::vec3>> solidPolygons(const Solid& solid);

bool solidBounds(const Solid& solid, glm::vec3& min, glm::vec3& max);

collision::Brush solidBrush(const Solid& solid);

enum class RampDir { PlusX, MinusX, PlusY, MinusY };

inline constexpr const char* kDefaultBlockMaterial = "DEV/DEV_MEASUREGENERIC01B";
inline constexpr const char* kDefaultRampMaterial = "DEV/DEV_MEASUREGENERIC01";
inline constexpr const char* kTriggerMaterial = "TOOLS/TOOLSTRIGGER";

Solid makeBlock(Document& doc, const glm::vec3& min, const glm::vec3& max, const std::string& material);
// a wedge filling the box, full height at `dir`'s side, zero at the other. steeper than 45 degrees is surfable
Solid makeRamp(Document& doc, const glm::vec3& min, const glm::vec3& max, RampDir dir, const std::string& material);

// hammer's world aligned texture axes for this face's orientation
void worldAlignTexture(Side& side);

// moves a solid, adjusting texture shifts so textures stay locked to it
void translateSolid(Solid& solid, const glm::vec3& delta);
// turns a solid about an axis through `pivot`, taking the texture with it: the projection axes rotate too and the shifts absorb the pivot moving, so the texture stays where it was on the brush instead of sliding across it
void rotateSolid(Solid& solid, const glm::vec3& pivot, const glm::vec3& axis, float degrees);
// maps the solid's points from one bounding box to another, per axis
void fitSolidToBounds(Solid& solid, const glm::vec3& oldMin, const glm::vec3& oldMax, const glm::vec3& newMin,
                      const glm::vec3& newMax);

bool isToolMaterial(const std::string& material);
// brushes made only of these are volumes/compile hints, never collision
bool isNonSolidToolMaterial(const std::string& material);

using MaterialTriangles = std::map<std::string, std::vector<bsp::Vertex>>;

// triangles grouped by material. uvs follow the side's texture axes, in texels divided by `textureSize`, pass 1 to get raw texels and divide by each material's real size once it's known
void appendSolidTriangles(const Solid& solid, bool includeToolFaces, MaterialTriangles& out, float textureSize = 512.0f);

}  // namespace vmf
