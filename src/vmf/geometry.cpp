#include <glm/gtc/matrix_transform.hpp>
#include "vmf/geometry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace vmf {

namespace {

constexpr double kEpsilon = 0.01;

std::string upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// the windings are built in double, not float, and only narrowed on the way out. the starting quad below is 131072 units across and a float's 24 bit mantissa is good to about 1/128 of a unit at that size, an error that rides all the way down into the final vertex however small the brush. it showed up as a 2048 unit brush reporting itself 2048.004 wide once it had been moved somewhere that didn't round kindly. a double has ~2e-8 units of headroom at the same size so the result narrows to the exactly right float
struct WidePlane {
    glm::dvec3 normal;
    double dist;
};

WidePlane widen(const Plane& p) { return {glm::dvec3(p.normal), static_cast<double>(p.dist)}; }

// big quad on the plane, wound ccw around its normal
std::vector<glm::dvec3> baseWinding(const WidePlane& p) {
    glm::dvec3 n = p.normal;
    glm::dvec3 helper = std::abs(n.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
    glm::dvec3 u = glm::normalize(glm::cross(helper, n));
    glm::dvec3 v = glm::cross(n, u);
    glm::dvec3 c = n * p.dist;
    constexpr double kSize = 131072.0;
    u *= kSize;
    v *= kSize;
    return {c - u - v, c + u - v, c + u + v, c - u + v};
}

// keeps the part of the polygon on the inside of the plane
std::vector<glm::dvec3> clip(const std::vector<glm::dvec3>& poly, const WidePlane& p) {
    std::vector<glm::dvec3> out;
    if (poly.empty()) return out;
    for (size_t i = 0; i < poly.size(); ++i) {
        const glm::dvec3& a = poly[i];
        const glm::dvec3& b = poly[(i + 1) % poly.size()];
        double da = glm::dot(p.normal, a) - p.dist;
        double db = glm::dot(p.normal, b) - p.dist;
        if (da <= kEpsilon) out.push_back(a);
        if ((da < -kEpsilon && db > kEpsilon) || (da > kEpsilon && db < -kEpsilon)) {
            out.push_back(a + (b - a) * (da / (da - db)));
        }
    }
    return out;
}

std::vector<std::vector<glm::vec3>> polygonsForPlanes(const std::vector<Plane>& planes) {
    std::vector<WidePlane> wide;
    wide.reserve(planes.size());
    for (const auto& p : planes) wide.push_back(widen(p));

    std::vector<std::vector<glm::vec3>> polys(planes.size());
    for (size_t i = 0; i < wide.size(); ++i) {
        auto poly = baseWinding(wide[i]);
        for (size_t j = 0; j < wide.size() && !poly.empty(); ++j) {
            if (j == i) continue;
            // skip coincident duplicate planes so they don't erase each other
            if (glm::dot(wide[i].normal, wide[j].normal) > 0.9999 &&
                std::abs(wide[i].dist - wide[j].dist) < kEpsilon) {
                if (j < i) poly.clear();
                continue;
            }
            poly = clip(poly, wide[j]);
        }
        if (poly.size() < 3) continue;  // polys[i] is already empty
        polys[i].reserve(poly.size());
        for (const auto& v : poly) polys[i].push_back(glm::vec3(v));
    }
    return polys;
}

int polygonCount(const std::vector<std::vector<glm::vec3>>& polys) {
    return static_cast<int>(std::count_if(polys.begin(), polys.end(), [](const auto& p) { return !p.empty(); }));
}

// corner i: bit 0 = x, bit 1 = y, bit 2 = top. a ramp is the box with its low side's top corners pulled down to the floor
std::array<glm::vec3, 8> shapeCorners(const glm::vec3& min, const glm::vec3& max, bool ramp, RampDir dir) {
    std::array<glm::vec3, 8> c;
    int axis = (dir == RampDir::PlusX || dir == RampDir::MinusX) ? 0 : 1;
    bool highAtMax = dir == RampDir::PlusX || dir == RampDir::PlusY;
    for (int i = 0; i < 8; ++i) {
        c[i] = glm::vec3((i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z);
        if (ramp && (i & 4)) {
            bool atMax = axis == 0 ? (i & 1) != 0 : (i & 2) != 0;
            if (atMax != highAtMax) c[i].z = min.z;
        }
    }
    return c;
}

Solid makeShape(Document& doc, const glm::vec3& minIn, const glm::vec3& maxIn, bool ramp, RampDir dir,
                const std::string& material) {
    glm::vec3 min = glm::min(minIn, maxIn), max = glm::max(minIn, maxIn);
    auto c = shapeCorners(min, max, ramp, dir);
    struct Face {
        std::array<int, 4> idx;
        glm::vec3 outward;
    };
    static constexpr std::array<Face, 6> kFaces = {{
        {{0, 2, 6, 4}, {-1, 0, 0}},
        {{1, 3, 7, 5}, {1, 0, 0}},
        {{0, 1, 5, 4}, {0, -1, 0}},
        {{2, 3, 7, 6}, {0, 1, 0}},
        {{0, 1, 3, 2}, {0, 0, -1}},
        {{4, 5, 7, 6}, {0, 0, 1}},
    }};

    Solid solid;
    solid.id = doc.allocateId();
    for (const auto& f : kFaces) {
        // three corners of the face that aren't collinear (a ramp's side faces have two coincident corners, its low end has no area at all)
        std::array<glm::vec3, 4> q = {c[f.idx[0]], c[f.idx[1]], c[f.idx[2]], c[f.idx[3]]};
        bool found = false;
        std::array<glm::vec3, 3> pts{};
        for (int skip = 3; skip >= 0 && !found; --skip) {
            int k = 0;
            for (int i = 0; i < 4; ++i) {
                if (i != skip) pts[k++] = q[i];
            }
            glm::vec3 n = glm::cross(pts[0] - pts[1], pts[2] - pts[1]);
            if (glm::length(n) > 1e-3f) {
                if (glm::dot(n, f.outward) < 0.0f) std::swap(pts[0], pts[2]);
                found = true;
            }
        }
        if (!found) continue;
        Side side;
        side.id = doc.allocateId();
        side.points = pts;
        side.material = material;
        worldAlignTexture(side);
        solid.sides.push_back(side);
    }
    return solid;
}

}  // namespace

Plane sidePlane(const Side& side) {
    glm::vec3 n = glm::cross(side.points[0] - side.points[1], side.points[2] - side.points[1]);
    float len = glm::length(n);
    if (len < 1e-6f) return {{0, 0, 1}, 0.0f};
    n /= len;
    return {n, glm::dot(n, side.points[0])};
}

std::vector<Plane> solidPlanes(const Solid& solid) {
    std::vector<Plane> planes;
    planes.reserve(solid.sides.size());
    for (const auto& side : solid.sides) planes.push_back(sidePlane(side));
    if (polygonCount(polygonsForPlanes(planes)) < 4) {
        std::vector<Plane> flipped = planes;
        for (auto& p : flipped) p = {-p.normal, -p.dist};
        if (polygonCount(polygonsForPlanes(flipped)) >= 4) return flipped;
    }
    return planes;
}

std::vector<std::vector<glm::vec3>> solidPolygons(const Solid& solid) {
    return polygonsForPlanes(solidPlanes(solid));
}

bool solidBounds(const Solid& solid, glm::vec3& min, glm::vec3& max) {
    bool any = false;
    for (const auto& poly : solidPolygons(solid)) {
        for (const auto& p : poly) {
            if (!any) {
                min = max = p;
                any = true;
            }
            min = glm::min(min, p);
            max = glm::max(max, p);
        }
    }
    return any;
}

collision::Brush solidBrush(const Solid& solid) {
    collision::Brush brush;
    for (const auto& p : solidPlanes(solid)) brush.sides.push_back({p.normal, p.dist});
    // bevels, like vbsp's AddBrushBevels. box collision pushes each plane out by the box's size, which near a slanted face's edge or corner claims space the brush doesn't have ("bumping into nothing" at ramp edges). extra planes through the edges cut that space away: axial ones at the bounds, and for every edge the planes along it facing each axis that have the whole brush behind them
    glm::vec3 min, max;
    if (!solidBounds(solid, min, max)) return brush;
    // the axial ones always go in exactly axial (brushBounds reads them), even beside a face that's axial give or take float error
    brush.sides.push_back({{1, 0, 0}, max.x});
    brush.sides.push_back({{-1, 0, 0}, -min.x});
    brush.sides.push_back({{0, 1, 0}, max.y});
    brush.sides.push_back({{0, -1, 0}, -min.y});
    brush.sides.push_back({{0, 0, 1}, max.z});
    brush.sides.push_back({{0, 0, -1}, -min.z});
    auto addPlane = [&](const glm::vec3& n, float d) {
        for (const auto& s : brush.sides) {
            if (glm::dot(s.normal, n) > 0.9999f && std::abs(s.dist - d) < 0.01f) return;
        }
        brush.sides.push_back({n, d});
    };

    std::vector<std::vector<glm::vec3>> polygons = solidPolygons(solid);
    std::vector<glm::vec3> corners;
    for (const auto& poly : polygons) corners.insert(corners.end(), poly.begin(), poly.end());
    const glm::vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const auto& poly : polygons) {
        for (size_t i = 0; i < poly.size(); ++i) {
            glm::vec3 edge = poly[(i + 1) % poly.size()] - poly[i];
            if (glm::length(edge) < 0.5f) continue;
            edge = glm::normalize(edge);
            for (const auto& axis : axes) {
                for (float sign : {1.0f, -1.0f}) {
                    glm::vec3 n = glm::cross(edge, axis * sign);
                    float len = glm::length(n);
                    if (len < 0.1f) continue;  // edge (nearly) along this axis
                    n /= len;
                    // within a hair of an axis it adds nothing to the axial ones (and would read as one, a little off, to brushBounds)
                    glm::vec3 an = glm::abs(n);
                    if (std::max(an.x, std::max(an.y, an.z)) > 0.9999f) continue;
                    float d = glm::dot(n, poly[i]);
                    bool supporting = true;
                    for (const auto& c : corners) {
                        if (glm::dot(n, c) - d > 0.1f) {
                            supporting = false;
                            break;
                        }
                    }
                    if (supporting) addPlane(n, d);
                }
            }
        }
    }
    return brush;
}

Solid makeBlock(Document& doc, const glm::vec3& min, const glm::vec3& max, const std::string& material) {
    return makeShape(doc, min, max, false, RampDir::PlusX, material);
}

Solid makeRamp(Document& doc, const glm::vec3& min, const glm::vec3& max, RampDir dir, const std::string& material) {
    return makeShape(doc, min, max, true, dir, material);
}

void worldAlignTexture(Side& side) {
    glm::vec3 n = glm::abs(sidePlane(side).normal);
    if (n.z >= n.x && n.z >= n.y) {
        side.uAxis = {1, 0, 0};
        side.vAxis = {0, -1, 0};
    } else if (n.x >= n.y) {
        side.uAxis = {0, 1, 0};
        side.vAxis = {0, 0, -1};
    } else {
        side.uAxis = {1, 0, 0};
        side.vAxis = {0, 0, -1};
    }
    side.uShift = side.vShift = 0.0f;
}

void translateSolid(Solid& solid, const glm::vec3& delta) {
    for (auto& side : solid.sides) {
        for (auto& p : side.points) p += delta;
        if (side.uScale != 0.0f) side.uShift -= glm::dot(delta, side.uAxis) / side.uScale;
        if (side.vScale != 0.0f) side.vShift -= glm::dot(delta, side.vAxis) / side.vScale;
    }
}

void rotateSolid(Solid& solid, const glm::vec3& pivot, const glm::vec3& axis, float degrees) {
    if (glm::length(axis) < 1e-6f) return;
    const glm::mat3 r = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(degrees), glm::normalize(axis)));
    for (auto& side : solid.sides) {
        for (auto& p : side.points) p = r * (p - pivot) + pivot;
        // texel u is dot(p, uAxis)/uScale + uShift. rotating both p and uAxis leaves dot(p - pivot, uAxis) alone so only the pivot's own contribution changes and the shift has to take up the difference
        const glm::vec3 newU = r * side.uAxis;
        const glm::vec3 newV = r * side.vAxis;
        if (side.uScale != 0.0f) side.uShift += (glm::dot(pivot, side.uAxis) - glm::dot(pivot, newU)) / side.uScale;
        if (side.vScale != 0.0f) side.vShift += (glm::dot(pivot, side.vAxis) - glm::dot(pivot, newV)) / side.vScale;
        side.uAxis = newU;
        side.vAxis = newV;
    }
}

void fitSolidToBounds(Solid& solid, const glm::vec3& oldMin, const glm::vec3& oldMax, const glm::vec3& newMin,
                      const glm::vec3& newMax) {
    glm::vec3 oldSize = oldMax - oldMin;
    glm::vec3 newSize = newMax - newMin;
    for (auto& side : solid.sides) {
        for (auto& p : side.points) {
            for (int a = 0; a < 3; ++a) {
                float t = oldSize[a] > 1e-6f ? (p[a] - oldMin[a]) / oldSize[a] : 0.0f;
                p[a] = newMin[a] + t * newSize[a];
            }
        }
    }
}

bool isToolMaterial(const std::string& material) {
    return upper(material).rfind("TOOLS/", 0) == 0;
}

bool isNonSolidToolMaterial(const std::string& material) {
    static const std::array<const char*, 7> kNonSolid = {
        "TOOLS/TOOLSHINT",    "TOOLS/TOOLSSKIP", "TOOLS/TOOLSTRIGGER", "TOOLS/TOOLSAREAPORTAL",
        "TOOLS/TOOLSOCCLUDER", "TOOLS/TOOLSFOG", "TOOLS/TOOLSLIGHT",
    };
    std::string m = upper(material);
    for (const char* t : kNonSolid) {
        if (m == t) return true;
    }
    return false;
}

void appendSolidTriangles(const Solid& solid, bool includeToolFaces, MaterialTriangles& out, float textureSize) {
    auto polys = solidPolygons(solid);
    auto planes = solidPlanes(solid);
    for (size_t i = 0; i < solid.sides.size(); ++i) {
        const Side& side = solid.sides[i];
        const auto& poly = polys[i];
        if (poly.size() < 3) continue;
        if (!includeToolFaces && isToolMaterial(side.material)) continue;
        auto& tris = out[side.material];
        glm::vec3 n = planes[i].normal;
        auto vertex = [&](const glm::vec3& p) {
            float u = (glm::dot(p, side.uAxis) / (side.uScale != 0.0f ? side.uScale : 0.25f) + side.uShift) / textureSize;
            float v = (glm::dot(p, side.vAxis) / (side.vScale != 0.0f ? side.vScale : 0.25f) + side.vShift) / textureSize;
            return bsp::Vertex{p, n, {u, v}};
        };
        for (size_t k = 1; k + 1 < poly.size(); ++k) {
            tris.push_back(vertex(poly[0]));
            tris.push_back(vertex(poly[k]));
            tris.push_back(vertex(poly[k + 1]));
        }
    }
}

}  // namespace vmf
