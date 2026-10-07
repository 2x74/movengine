#include "collision/bsp_brush_loader.h"

#include <bsppp/BSP.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace collision {

namespace {

// bsppp has no struct support for BRUSHES/BRUSHSIDES/LEAFS/LEAFBRUSHES (confirmed by reading its LumpData.h, only the render relevant lumps get typed parsers) so these are hand rolled from the public bsp(source) format spec
#pragma pack(push, 1)
struct RawBrush {
    int32_t firstSide;
    int32_t numSides;
    int32_t contents;
};
struct RawBrushSide {
    uint16_t planeNum;
    int16_t texInfo;
    int16_t dispInfo;
    int16_t bevel;
};
#pragma pack(pop)

constexpr int32_t CONTENTS_SOLID = 0x1;
constexpr int32_t CONTENTS_WINDOW = 0x2;
constexpr int32_t CONTENTS_GRATE = 0x8;

// CONTENTS_PLAYERCLIP deliberately excluded: it's an invisible, often very large out of bounds volume (community bhop maps commonly wrap the whole legal play area in one to stop exploits), not real geometry. treating it as solid meant standing in ordinary open space near its boundary read as "embedded" with the nearest real face hundreds of units away, exactly the "would need a 625 unit push" false positives this was producing. SOLID/WINDOW/GRATE are all things you'd actually see and could physically get stuck against so those stay
bool isSolidForPlayer(int32_t contents) {
    return (contents & (CONTENTS_SOLID | CONTENTS_WINDOW | CONTENTS_GRATE)) != 0;
}

// real walls/floors/ceilings/pillars are always thin in at least one dimension, a few dozen units, never more than a couple hundred. some "_fix" community maps (this one included) add a single giant box brush enclosing the whole playable area as a last resort anti exploit backstop, meant to only matter if a real engine's swept collision catches you crossing its boundary. our collision is point in brush, not swept, so merely existing anywhere inside that box (everywhere you can legally stand) reads as "penetrating" and the resolver shoves you toward whichever face happens to be nearest (confirmed: a clean 6 sided 6032x1677x1563 unit box here, not a parsing bug). thick in all three axes at once is the actual signature of that kind of volume vs real geometry, so that's what gets filtered, not another contents flag guess
constexpr float kMaxTrustedThinDimension = 1000.0f;

bool looksLikeEnclosingBound(const Brush& brush) {
    constexpr glm::vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const auto& axis : axes) {
        bool foundPlus = false, foundMinus = false;
        float distPlus = 0.0f, distMinus = 0.0f;
        for (const auto& side : brush.sides) {
            if (glm::dot(side.normal, axis) > 0.999f) {
                foundPlus = true;
                distPlus = side.dist;
            } else if (glm::dot(side.normal, axis) < -0.999f) {
                foundMinus = true;
                distMinus = side.dist;
            }
        }
        // no matching axis aligned plane on this axis (an angled/non box brush), can't judge thinness this way, don't filter it
        if (!foundPlus || !foundMinus) {
            return false;
        }
        if (distPlus + distMinus < kMaxTrustedThinDimension) {
            return false;  // thin enough on this axis to be real geometry
        }
    }
    return true;  // thick on every axis, an enclosing volume not a wall
}

// brush entity models are compiled relative to the entity's origin (and the engine applies its angles on top), not in world space. verified against both bundled maps, where every trigger/func_brush model's brush extents are centered on zero
glm::mat3 entityRotation(const MapEntity& e) { return sourceAngleMatrix(parseKeyValueVec3(e.value("angles"))); }
glm::vec3 entityOrigin(const MapEntity& e) { return parseKeyValueVec3(e.value("origin")); }

std::vector<MapEntity> readEntities(const bsppp::BSP& file) {
    std::vector<MapEntity> out;
    for (const auto& entity : file.getLumpData<bsppp::BSPLump::ENTITIES>()) {
        MapEntity info;
        for (const auto& kv : entity.getKeyValues()) {
            info.keyValues.emplace_back(toLowerAscii(kv.getKey()), std::string(kv.getValue()));
        }
        out.push_back(std::move(info));
    }
    return out;
}

// "*N" -> N for brush entities, -1 for everything else.
int brushModelIndex(const MapEntity& e) {
    std::string model = e.value("model");
    if (model.size() < 2 || model[0] != '*') return -1;
    return parseKeyValueInt(model.substr(1), -1);
}

struct BrushLumps {
    std::vector<Plane> planes;
    std::vector<RawBrush> brushes;
    std::vector<RawBrushSide> sides;
    std::vector<int> ownerModel;  // per brush: which brush model's BSP tree references it (0 = worldspawn)
};

// brushes don't record which model they belong to, the only link is each model's own bsp tree, whose leaves list the brushes inside them
std::vector<int> computeBrushOwners(const bsppp::BSP& file, size_t numBrushes) {
    std::vector<int> owner(numBrushes, -1);
    auto nodes = file.getLumpData<bsppp::BSPLump::NODES>();
    auto models = file.getLumpData<bsppp::BSPLump::MODELS>();
    auto leafBytes = file.getLumpData(bsppp::BSPLump::LEAFS);
    auto leafBrushBytes = file.getLumpData(bsppp::BSPLump::LEAFBRUSHES);
    if (nodes.empty() || models.empty() || !leafBytes || !leafBrushBytes) {
        return owner;
    }

    // dleaf_t v0 carries a 24 byte ambient light cube (56 bytes total), v1 dropped it (32 bytes), firstleafbrush/numleafbrushes are at offset 24 in both
    const size_t leafStride = file.getLumpVersion(bsppp::BSPLump::LEAFS) == 0 ? 56 : 32;
    const size_t numLeafs = leafBytes->size() / leafStride;
    const size_t numLeafBrushes = leafBrushBytes->size() / sizeof(uint16_t);

    std::vector<int32_t> stack;
    for (size_t m = 0; m < models.size(); ++m) {
        stack.assign(1, models[m].headNode);
        while (!stack.empty()) {
            int32_t n = stack.back();
            stack.pop_back();
            if (n < 0) {
                size_t leaf = static_cast<size_t>(-1 - n);
                if (leaf >= numLeafs) continue;
                uint16_t first = 0, count = 0;
                std::memcpy(&first, leafBytes->data() + leaf * leafStride + 24, sizeof(first));
                std::memcpy(&count, leafBytes->data() + leaf * leafStride + 26, sizeof(count));
                for (size_t k = first; k < static_cast<size_t>(first) + count && k < numLeafBrushes; ++k) {
                    uint16_t b = 0;
                    std::memcpy(&b, leafBrushBytes->data() + k * sizeof(uint16_t), sizeof(b));
                    if (b < numBrushes && owner[b] == -1) {
                        owner[b] = static_cast<int>(m);
                    }
                }
            } else if (static_cast<size_t>(n) < nodes.size()) {
                stack.push_back(nodes[static_cast<size_t>(n)].children[0]);
                stack.push_back(nodes[static_cast<size_t>(n)].children[1]);
            }
        }
    }
    return owner;
}

std::optional<BrushLumps> readBrushLumps(const bsppp::BSP& file, const std::string& path) {
    auto planes = file.getLumpData<bsppp::BSPLump::PLANES>();
    auto rawBrushBytes = file.getLumpData(bsppp::BSPLump::BRUSHES);
    auto rawSideBytes = file.getLumpData(bsppp::BSPLump::BRUSHSIDES);
    if (planes.empty() || !rawBrushBytes || !rawSideBytes) {
        std::fprintf(stderr, "bsp is missing brush lumps: %s\n", path.c_str());
        return std::nullopt;
    }

    BrushLumps out;
    out.planes.reserve(planes.size());
    for (const auto& p : planes) {
        out.planes.push_back({glm::vec3(p.normal[0], p.normal[1], p.normal[2]), p.dist});
    }

    out.brushes.resize(rawBrushBytes->size() / sizeof(RawBrush));
    std::memcpy(out.brushes.data(), rawBrushBytes->data(), out.brushes.size() * sizeof(RawBrush));
    out.sides.resize(rawSideBytes->size() / sizeof(RawBrushSide));
    std::memcpy(out.sides.data(), rawSideBytes->data(), out.sides.size() * sizeof(RawBrushSide));

    out.ownerModel = computeBrushOwners(file, out.brushes.size());
    return out;
}

// local space planes -> world: for p_world = R * p_local + origin, the plane (n, d) becomes (R n, d + dot(R n, origin))
Brush buildBrush(const BrushLumps& lumps, size_t brushIndex, const glm::mat3& rotation, const glm::vec3& origin) {
    const RawBrush& rb = lumps.brushes[brushIndex];
    Brush brush;
    brush.sides.reserve(static_cast<size_t>(std::max(rb.numSides, 0)));
    for (int32_t i = 0; i < rb.numSides; ++i) {
        size_t sideIndex = static_cast<size_t>(rb.firstSide + i);
        if (sideIndex >= lumps.sides.size()) continue;
        uint16_t planeNum = lumps.sides[sideIndex].planeNum;
        if (planeNum >= lumps.planes.size()) continue;
        const Plane& p = lumps.planes[planeNum];
        glm::vec3 n = rotation * p.normal;
        brush.sides.push_back({n, p.dist + glm::dot(n, origin)});
    }
    return brush;
}

}  // namespace

std::optional<WorldBrushes> loadWorldBrushes(const std::string& path) {
    bsppp::BSP file(path);
    if (!file) {
        std::fprintf(stderr, "failed to open bsp: %s\n", path.c_str());
        return std::nullopt;
    }
    auto lumps = readBrushLumps(file, path);
    if (!lumps) {
        return std::nullopt;
    }

    std::unordered_map<int, MapEntity> entityByModel;
    for (auto& e : readEntities(file)) {
        int model = brushModelIndex(e);
        if (model > 0) {
            entityByModel.emplace(model, std::move(e));
        }
    }

    // brush entities (doors, func_brush, ...) are still treated as permanently static solid geometry at their spawn position, doors and plats just sit there, the accepted limitation for now. non solid ones (triggers, illusionary, rain volumes) are left out entirely
    WorldBrushes world;
    world.brushes.reserve(lumps->brushes.size());
    size_t skippedNonSolidEntityBrushes = 0;

    for (size_t i = 0; i < lumps->brushes.size(); ++i) {
        if (!isSolidForPlayer(lumps->brushes[i].contents)) {
            continue;
        }
        glm::mat3 rotation(1.0f);
        glm::vec3 origin(0.0f);
        int model = lumps->ownerModel[i];
        if (model < 0) {
            // in no model's bsp tree, so no trace in the engine ever reaches it (surf_kitsune has a stray one right at spawn)
            continue;
        }
        if (model > 0) {
            auto it = entityByModel.find(model);
            if (it != entityByModel.end()) {
                if (!brushEntitySolidForPlayer(it->second)) {
                    skippedNonSolidEntityBrushes++;
                    continue;
                }
                rotation = entityRotation(it->second);
                origin = entityOrigin(it->second);
            }
        }
        Brush brush = buildBrush(*lumps, i, rotation, origin);
        if (!brush.sides.empty() && !looksLikeEnclosingBound(brush)) {
            world.brushes.push_back(std::move(brush));
        }
    }

    std::printf("loaded %zu solid brushes (of %zu total, %zu non-solid entity brushes skipped)\n",
                world.brushes.size(), lumps->brushes.size(), skippedNonSolidEntityBrushes);
    return world;
}

std::optional<MapTriggers> loadMapTriggers(const std::string& path) {
    bsppp::BSP file(path);
    if (!file) {
        std::fprintf(stderr, "failed to open bsp for triggers: %s\n", path.c_str());
        return std::nullopt;
    }
    auto lumps = readBrushLumps(file, path);
    if (!lumps) {
        return std::nullopt;
    }
    std::vector<MapEntity> entities = readEntities(file);

    std::unordered_map<int, std::vector<size_t>> brushesByModel;
    for (size_t i = 0; i < lumps->ownerModel.size(); ++i) {
        if (lumps->ownerModel[i] > 0) {
            brushesByModel[lumps->ownerModel[i]].push_back(i);
        }
    }
    for (auto& e : entities) {
        auto it = brushesByModel.find(brushModelIndex(e));
        if (it == brushesByModel.end()) continue;
        glm::mat3 rotation = entityRotation(e);
        glm::vec3 origin = entityOrigin(e);
        for (size_t b : it->second) {
            Brush brush = buildBrush(*lumps, b, rotation, origin);
            if (!brush.sides.empty()) e.brushes.push_back(std::move(brush));
        }
    }
    return buildMapTriggers(entities);
}

std::optional<std::vector<BspBrush>> loadBspBrushes(const std::string& path) {
    bsppp::BSP file(path);
    if (!file) return std::nullopt;
    auto lumps = readBrushLumps(file, path);
    if (!lumps) return std::nullopt;
    std::vector<BspBrush> out(lumps->brushes.size());
    for (size_t i = 0; i < lumps->brushes.size(); ++i) {
        const RawBrush& rb = lumps->brushes[i];
        out[i].contents = rb.contents;
        out[i].model = lumps->ownerModel[i];
        for (int32_t k = 0; k < rb.numSides; ++k) {
            size_t sideIndex = static_cast<size_t>(rb.firstSide + k);
            if (sideIndex >= lumps->sides.size()) continue;
            const RawBrushSide& rs = lumps->sides[sideIndex];
            if (rs.planeNum >= lumps->planes.size()) continue;
            out[i].sides.push_back({lumps->planes[rs.planeNum], rs.texInfo, rs.dispInfo, rs.bevel != 0});
        }
    }
    return out;
}

}  // namespace collision
