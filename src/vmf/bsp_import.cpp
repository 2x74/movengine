#include "vmf/bsp_import.h"

#include <bsppp/BSP.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

#include "collision/bsp_brush_loader.h"
#include "collision/map_triggers.h"
#include "vmf/geometry.h"

namespace vmf {

namespace {

constexpr int kContentsAreaPortal = 0x8000;
constexpr int kContentsDetail = 0x8000000;
constexpr int kContentsPlayerClip = 0x10000;
constexpr int kContentsMonsterClip = 0x20000;

// vbsp throws a clip brush's texture away, the faces come back pointing at nodraw and only the brush's contents still say what it was. without this a decompiled map has no clip brushes in it at all: they look like any other nodraw brush so you can't see them, select them by material or clear them out, and recompiling turns them into solid walls
const char* clipMaterialFor(int contents) {
    const bool player = (contents & kContentsPlayerClip) != 0;
    const bool monster = (contents & kContentsMonsterClip) != 0;
    if (player && monster) return "TOOLS/TOOLSCLIP";
    if (player) return "TOOLS/TOOLSPLAYERCLIP";
    if (monster) return "TOOLS/TOOLSNPCCLIP";
    return nullptr;
}

std::vector<std::string> texdataNames(bsppp::BSP& file) {
    std::vector<std::string> names;
    auto table = file.getLumpData(bsppp::BSPLump::TEXDATA_STRING_TABLE);
    auto data = file.getLumpData(bsppp::BSPLump::TEXDATA_STRING_DATA);
    if (!table || !data) return names;
    size_t count = table->size() / sizeof(int32_t);
    for (size_t i = 0; i < count; ++i) {
        int32_t offset = 0;
        std::memcpy(&offset, table->data() + i * sizeof(int32_t), sizeof(offset));
        if (offset < 0 || static_cast<size_t>(offset) >= data->size()) {
            names.emplace_back();
            continue;
        }
        const char* begin = reinterpret_cast<const char*>(data->data()) + offset;
        names.emplace_back(begin, strnlen(begin, data->size() - static_cast<size_t>(offset)));
    }
    return names;
}

bool isOutputKey(const std::string& key, const std::string& value) {
    if (key.size() < 3 || std::tolower(static_cast<unsigned char>(key[0])) != 'o' ||
        std::tolower(static_cast<unsigned char>(key[1])) != 'n') {
        return false;
    }
    return value.find('\x1b') != std::string::npos || std::count(value.begin(), value.end(), ',') >= 4;
}

// corners come out of float plane intersections a hair off, back to the whole numbers they almost always were
float snap(float v) {
    float r = std::round(v);
    return std::abs(v - r) < 0.05f ? r : v;
}

}  // namespace

std::string unpatchMaterialName(const std::string& name) {
    std::string lower = collision::toLowerAscii(name);
    std::string out = name;
    if (lower.rfind("maps/", 0) == 0) {
        size_t slash = lower.find('/', 5);
        if (slash != std::string::npos) {
            out = out.substr(slash + 1);
            lower = lower.substr(slash + 1);
        }
    }
    const std::string wvt = "_wvt_patch";
    if (lower.size() > wvt.size() && lower.compare(lower.size() - wvt.size(), wvt.size(), wvt) == 0) {
        out.resize(out.size() - wvt.size());
        lower.resize(lower.size() - wvt.size());
    }
    // trailing "_x_y_z" cubemap position (integers, maybe negative)
    size_t end = lower.size();
    int numbers = 0;
    while (numbers < 3) {
        size_t underscore = lower.rfind('_', end - 1);
        if (underscore == std::string::npos || underscore + 1 >= end) break;
        std::string part = lower.substr(underscore + 1, end - underscore - 1);
        size_t digits = part[0] == '-' ? 1 : 0;
        if (digits >= part.size() ||
            !std::all_of(part.begin() + static_cast<long>(digits), part.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            break;
        }
        end = underscore;
        ++numbers;
    }
    if (numbers == 3) out.resize(end);
    return out;
}

std::optional<Document> importBsp(const std::string& path) {
    bsppp::BSP file(path);
    if (!file) return std::nullopt;
    auto brushes = collision::loadBspBrushes(path);
    if (!brushes) return std::nullopt;
    auto texInfos = file.getLumpData<bsppp::BSPLump::TEXINFO>();
    auto texDatas = file.getLumpData<bsppp::BSPLump::TEXDATA>();
    std::vector<std::string> names = texdataNames(file);

    Document doc = emptyDocument();
    doc.world.keyValues.clear();
    doc.world.solids.clear();
    doc.world.id = doc.allocateId();

    auto materialOf = [&](int texInfo) -> std::string {
        if (texInfo < 0 || static_cast<size_t>(texInfo) >= texInfos.size()) return "TOOLS/TOOLSNODRAW";
        int td = texInfos[static_cast<size_t>(texInfo)].textureData;
        if (td < 0 || static_cast<size_t>(td) >= texDatas.size()) return "TOOLS/TOOLSNODRAW";
        int nameId = texDatas[static_cast<size_t>(td)].nameStringTableID;
        if (nameId < 0 || static_cast<size_t>(nameId) >= names.size()) return "TOOLS/TOOLSNODRAW";
        return unpatchMaterialName(names[static_cast<size_t>(nameId)]);
    };

    // one compiled brush -> one solid, moved into world space
    int droppedSides = 0;
    auto makeSolid = [&](const collision::BspBrush& b, const glm::mat3& rotation,
                         const glm::vec3& origin) -> std::optional<Solid> {
        // the middle, from the axial planes in the model's own space (a rotated entity has none in world space)
        collision::Brush local;
        for (const auto& s : b.sides) local.sides.push_back(s.plane);
        glm::vec3 mn, mx;
        if (b.sides.size() < 4 || !collision::brushBounds(local, mn, mx)) return std::nullopt;
        glm::vec3 center = rotation * ((mn + mx) * 0.5f) + origin;

        std::vector<collision::BspBrushSide> sides;
        for (auto s : b.sides) {
            glm::vec3 n = rotation * s.plane.normal;
            s.plane = {n, s.plane.dist + glm::dot(n, origin)};
            // bevels usually only touch the brush along an edge (no area, dropped below) but some do trim it, keep those like the game
            sides.push_back(s);
        }

        // any three points on each plane, wound so the normal points out, then the face's real corners replace them (snapped back to the whole numbers they almost always were in the source map)
        Solid solid;
        solid.id = doc.allocateId();
        for (const auto& s : sides) {
            glm::vec3 n = s.plane.normal;
            glm::vec3 p = center - n * (glm::dot(n, center) - s.plane.dist);
            glm::vec3 t1 = glm::normalize(glm::cross(n, std::abs(n.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0)));
            glm::vec3 t2 = glm::cross(n, t1);
            Side side;
            side.points = {p + t1 * 64.0f, p, p + t2 * 64.0f};
            side.material = materialOf(s.texInfo);
            // only where the compiler left nothing, a brush that kept a real texture keeps it
            if (const char* clip = clipMaterialFor(b.contents);
                clip && side.material == "TOOLS/TOOLSNODRAW") {
                side.material = clip;
            }
            if (s.texInfo >= 0 && static_cast<size_t>(s.texInfo) < texInfos.size()) {
                const auto& ti = texInfos[static_cast<size_t>(s.texInfo)];
                auto axis = [&](const sourcepp::math::Vec4f& v, glm::vec3& outAxis, float& shift, float& scale) {
                    glm::vec3 a = rotation * glm::vec3(v[0], v[1], v[2]);
                    float len = glm::length(a);
                    if (len < 1e-6f) return;
                    outAxis = a / len;
                    scale = 1.0f / len;
                    // the texture moves with the entity, re-base the shift on the world origin
                    shift = v[3] - glm::dot(a, origin);
                };
                axis(ti.textureVector1, side.uAxis, side.uShift, side.uScale);
                axis(ti.textureVector2, side.vAxis, side.vShift, side.vScale);
                float lm = glm::length(glm::vec3(ti.lightmapVector1[0], ti.lightmapVector1[1], ti.lightmapVector1[2]));
                if (lm > 1e-6f) side.lightmapScale = std::max(1, static_cast<int>(std::lround(1.0f / lm)));
            }
            solid.sides.push_back(std::move(side));
        }
        auto polygons = solidPolygons(solid);
        std::vector<Side> kept;
        for (size_t i = 0; i < solid.sides.size(); ++i) {
            const auto& poly = polygons[i];
            float area = 0.0f;
            for (size_t j = 2; j < poly.size(); ++j) area += glm::length(glm::cross(poly[j - 1] - poly[0], poly[j] - poly[0]));
            if (poly.size() < 3 || area < 0.5f) {
                if (!sides[i].bevel) ++droppedSides;  // a plane that doesn't really touch the brush
                continue;
            }
            // the widest triangle from the corners, for a stable plane
            std::vector<glm::vec3> pts;
            for (const auto& v : poly) pts.push_back({snap(v.x), snap(v.y), snap(v.z)});
            size_t a = 0, b = 1, c = 2;
            float best = -1.0f;
            for (size_t j = 1; j < pts.size(); ++j) {
                for (size_t k = j + 1; k < pts.size(); ++k) {
                    float area = glm::length(glm::cross(pts[j] - pts[0], pts[k] - pts[0]));
                    if (area > best) best = area, b = j, c = k;
                }
            }
            Side side = solid.sides[i];
            glm::vec3 want = sides[i].plane.normal;
            side.points = {pts[a], pts[b], pts[c]};
            // an axial face's points sit exactly on its plane so it stays exactly axial (a slight tilt moves far corners by units)
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(want[axis]) > 0.99999f) {
                    for (auto& p : side.points) p[axis] = sides[i].plane.dist * (want[axis] > 0.0f ? 1.0f : -1.0f);
                }
            }
            if (glm::dot(glm::cross(side.points[0] - side.points[1], side.points[2] - side.points[1]), want) < 0.0f) {
                std::swap(side.points[0], side.points[2]);
            }
            side.id = doc.allocateId();
            kept.push_back(std::move(side));
        }
        if (kept.size() < 4) return std::nullopt;
        solid.sides = std::move(kept);
        return solid;
    };

    std::map<int, std::vector<const collision::BspBrush*>> byModel;
    for (const auto& b : *brushes) {
        if (b.model >= 0 && !(b.contents & kContentsAreaPortal)) byModel[b.model].push_back(&b);
    }

    Entity detail;
    detail.keyValues = {{"classname", "func_detail"}};
    int pointEntities = 0, brushEntities = 0;
    for (const auto& lumpEntity : file.getLumpData<bsppp::BSPLump::ENTITIES>()) {
        Entity e;
        int model = -1;
        for (const auto& kv : lumpEntity.getKeyValues()) {
            std::string key(kv.getKey()), value(kv.getValue());
            if (collision::toLowerAscii(key) == "model" && value.size() > 1 && value[0] == '*') {
                model = collision::parseKeyValueInt(value.substr(1), -1);
                continue;  // a VMF brush entity carries its brushes instead
            }
            if (isOutputKey(key, value)) {
                std::replace(value.begin(), value.end(), '\x1b', ',');
                e.connections.emplace_back(key, value);
            } else {
                e.keyValues.emplace_back(key, value);
            }
        }
        std::string cls = collision::toLowerAscii(e.get("classname"));
        if (cls == "worldspawn") {
            e.id = doc.world.id;
            for (const auto* b : byModel[0]) {
                auto solid = makeSolid(*b, glm::mat3(1.0f), glm::vec3(0.0f));
                if (!solid) continue;
                if (b->contents & kContentsDetail) detail.solids.push_back(std::move(*solid));
                else e.solids.push_back(std::move(*solid));
            }
            std::error_code ec;
            std::filesystem::path abs = std::filesystem::absolute(path, ec);
            e.set(kPakfileKey, ec ? path : abs.string());
            doc.world = std::move(e);
            continue;
        }
        e.id = doc.allocateId();
        if (model > 0) {
            glm::mat3 rotation = collision::sourceAngleMatrix(collision::parseKeyValueVec3(e.get("angles")));
            glm::vec3 origin = collision::parseKeyValueVec3(e.get("origin"));
            for (const auto* b : byModel[model]) {
                if (auto solid = makeSolid(*b, rotation, origin)) e.solids.push_back(std::move(*solid));
            }
            ++brushEntities;
        } else {
            ++pointEntities;
        }
        doc.entities.push_back(std::move(e));
    }
    if (!detail.solids.empty()) {
        detail.id = doc.allocateId();
        doc.entities.push_back(std::move(detail));
    }
    std::printf("bsp import: %zu world brushes, %d brush entities, %d point entities%s\n", doc.world.solids.size(),
                brushEntities, pointEntities, droppedSides ? " (some degenerate faces dropped)" : "");
    return doc;
}

}  // namespace vmf
