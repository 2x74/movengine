#include "collision/brush_polygons.h"

#include <cmath>

namespace collision {

namespace {

// big enough to cover any source map (±16384 units) before clipping
constexpr double kHuge = 65536.0;
// points this close to a plane count as on it, so neighbouring faces of the same brush agree on their shared edge instead of leaving a sliver
constexpr double kOnPlane = 0.01;

// the winding is carried in double and only narrowed on the way out. in float the quad above starts out past the point where a 24 bit mantissa can hold hundredths of a unit, and that error survives the clip into every vertex, same defect that had the editor reporting a moved 2048 unit brush as 2048.004 wide. double has the headroom to land on the exact answer
struct WidePlane {
    glm::dvec3 normal;
    double dist;
};

// sutherland-hodgman against one half space: keep dot(normal, p) <= dist
std::vector<glm::dvec3> clipToPlane(const std::vector<glm::dvec3>& poly, const WidePlane& plane) {
    std::vector<glm::dvec3> out;
    if (poly.empty()) return out;
    out.reserve(poly.size() + 1);
    for (size_t i = 0; i < poly.size(); ++i) {
        const glm::dvec3& prev = poly[(i + poly.size() - 1) % poly.size()];
        const glm::dvec3& cur = poly[i];
        double dPrev = glm::dot(plane.normal, prev) - plane.dist;
        double dCur = glm::dot(plane.normal, cur) - plane.dist;
        bool inPrev = dPrev <= kOnPlane;
        bool inCur = dCur <= kOnPlane;
        if (inPrev != inCur) {
            double denom = dPrev - dCur;
            if (std::fabs(denom) > 1e-12) {
                out.push_back(prev + (cur - prev) * (dPrev / denom));
            }
        }
        if (inCur) out.push_back(cur);
    }
    return out;
}

// twice the area of a polygon, via the newell cross product sum
double doubleArea(const std::vector<glm::dvec3>& poly) {
    glm::dvec3 sum(0.0);
    for (size_t i = 0; i < poly.size(); ++i) {
        sum += glm::cross(poly[i], poly[(i + 1) % poly.size()]);
    }
    return glm::length(sum);
}

// drops points the clip tolerance left sitting on top of each other, which would otherwise turn a collapsed face into a run of coincident vertices
void dropDuplicatePoints(std::vector<glm::dvec3>& poly) {
    std::vector<glm::dvec3> out;
    out.reserve(poly.size());
    for (size_t i = 0; i < poly.size(); ++i) {
        const glm::dvec3& cur = poly[i];
        if (out.empty() || glm::distance(out.back(), cur) > kOnPlane) out.push_back(cur);
    }
    if (out.size() >= 2 && glm::distance(out.front(), out.back()) <= kOnPlane) out.pop_back();
    poly.swap(out);
}

}  // namespace

std::vector<BrushFace> brushPolygons(const Brush& brush) {
    std::vector<BrushFace> faces;
    faces.reserve(brush.sides.size());
    std::vector<WidePlane> wide;
    wide.reserve(brush.sides.size());
    for (const auto& s : brush.sides) wide.push_back({glm::dvec3(s.normal), static_cast<double>(s.dist)});

    for (size_t i = 0; i < brush.sides.size(); ++i) {
        double len = glm::length(wide[i].normal);
        if (len < 1e-6) continue;
        glm::dvec3 n = wide[i].normal / len;
        double dist = wide[i].dist / len;

        // any axis not parallel to the normal gives a usable in plane basis
        glm::dvec3 up = std::fabs(n.z) < 0.9 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(1.0, 0.0, 0.0);
        glm::dvec3 t = glm::normalize(glm::cross(up, n));
        glm::dvec3 b = glm::cross(n, t);
        glm::dvec3 origin = n * dist;

        // wound ccw seen from outside (from +n looking back)
        std::vector<glm::dvec3> poly = {
            origin - t * kHuge - b * kHuge,
            origin + t * kHuge - b * kHuge,
            origin + t * kHuge + b * kHuge,
            origin - t * kHuge + b * kHuge,
        };
        for (size_t j = 0; j < wide.size() && poly.size() >= 3; ++j) {
            if (j == i) continue;
            poly = clipToPlane(poly, wide[j]);
        }
        dropDuplicatePoints(poly);
        // a side the others cut down to an edge (vbsp's bevels do this, and so does any plane that only touches the brush) survives the clip as a sliver a tolerance wide instead of vanishing. it has no area so it would draw nothing, drop it instead of carrying the triangles
        if (poly.size() >= 3 && doubleArea(poly) > 2.0) {
            BrushFace face;
            face.normal = glm::vec3(n);
            face.points.reserve(poly.size());
            for (const auto& v : poly) face.points.push_back(glm::vec3(v));
            faces.push_back(std::move(face));
        }
    }
    return faces;
}

}  // namespace collision
