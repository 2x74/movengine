#include "bsp/bsp_loader.h"

#include <bsppp/BSP.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace bsp {

namespace {

glm::vec3 toGlm(const sourcepp::math::Vec3f& v) {
    return {v[0], v[1], v[2]};
}

glm::vec4 toGlm(const sourcepp::math::Vec4f& v) {
    return {v[0], v[1], v[2], v[3]};
}

// TEXDATA_STRING_TABLE/TEXDATA_STRING_DATA have no bsppp typed parser (confirmed, same situation as BRUSHES/BRUSHSIDES in the collision module). table is a flat array of int32 offsets into the string blob
std::vector<std::string> parseTexdataStringTable(bsppp::BSP& file) {
    std::vector<std::string> names;

    auto tableBytes = file.getLumpData(bsppp::BSPLump::TEXDATA_STRING_TABLE);
    auto dataBytes = file.getLumpData(bsppp::BSPLump::TEXDATA_STRING_DATA);
    if (!tableBytes || !dataBytes) {
        return names;
    }

    size_t count = tableBytes->size() / sizeof(int32_t);
    std::vector<int32_t> offsets(count);
    std::memcpy(offsets.data(), tableBytes->data(), count * sizeof(int32_t));

    const char* blob = reinterpret_cast<const char*>(dataBytes->data());
    size_t blobSize = dataBytes->size();

    names.reserve(count);
    for (int32_t offset : offsets) {
        if (offset < 0 || static_cast<size_t>(offset) >= blobSize) {
            names.emplace_back();
            continue;
        }
        names.emplace_back(blob + offset);  // null-terminated within the blob
    }
    return names;
}

// packs every face's lightmap into one atlas, left to right in rows ("shelves"), growing downward. each lightmap gets a 1 luxel border copied from its edge so bilinear filtering doesn't bleed a neighbor's light in
class LightmapAtlas {
public:
    static constexpr int kWidth = 1024;
    // linear light that a full luxel stands for. source's ldr overbright is 2, so a surface lit to 1.0 sits mid range and lights can go to 2x that before they clip. must match kLightmapRange in the shader
    static constexpr float kLightmapRange = 2.0f;

    LightmapAtlas() {
        // faces with no lightmap: neutral linear 1.0, which encodes to round(pow(1/kLightmapRange, 1/2.2) * 255) = 187
        glm::ivec2 at = allocate(4, 4);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) set(at.x + x, at.y + y, {187, 187, 187});
    }

    glm::vec2 whiteTexel() const { return {2.0f, 2.0f}; }

    // copies a w x h block of ColorRGBExp32 luxels in, returns where luxel (0,0) landed in atlas pixels
    glm::ivec2 add(const uint8_t* luxels, int w, int h) {
        glm::ivec2 at = allocate(w + 2, h + 2);
        for (int y = -1; y <= h; ++y) {
            for (int x = -1; x <= w; ++x) {
                int sx = std::clamp(x, 0, w - 1), sy = std::clamp(y, 0, h - 1);
                const uint8_t* l = luxels + (static_cast<size_t>(sy) * w + sx) * 4;
                set(at.x + 1 + x, at.y + 1 + y, decode(l));
            }
        }
        return at + glm::ivec2(1);
    }

    DecodedTexture finish() {
        DecodedTexture t;
        t.width = kWidth;
        t.height = height_;
        t.rgba8888 = std::move(pixels_);
        return t;
    }
    int height() const { return height_; }

private:
    // source stores linear light as rgb with a shared exponent. keep it linear, scaled so kLightmapRange maps to 1.0, and gamma encode only to store it: 8 bits of linear light bands badly in the dark half of a map, and most of a map is the dark half. the shader undoes the encoding and multiplies back up so all the lighting math is linear
    static std::array<uint8_t, 3> decode(const uint8_t* l) {
        int8_t exponent = static_cast<int8_t>(l[3]);
        float scale = std::ldexp(1.0f, exponent) / 255.0f;
        std::array<uint8_t, 3> out{};
        for (int c = 0; c < 3; ++c) {
            float linear = l[c] * scale;
            float v = std::pow(std::clamp(linear / kLightmapRange, 0.0f, 1.0f), 1.0f / 2.2f);
            out[c] = static_cast<uint8_t>(std::round(v * 255.0f));
        }
        return out;
    }

    glm::ivec2 allocate(int w, int h) {
        if (cursorX_ + w > kWidth) {
            cursorX_ = 0;
            cursorY_ += rowHeight_;
            rowHeight_ = 0;
        }
        glm::ivec2 at(cursorX_, cursorY_);
        cursorX_ += w;
        rowHeight_ = std::max(rowHeight_, h);
        if (cursorY_ + h > height_) {
            height_ = cursorY_ + h;
            pixels_.resize(static_cast<size_t>(kWidth) * height_ * 4, 255);
        }
        return at;
    }

    void set(int x, int y, const std::array<uint8_t, 3>& rgb) {
        uint8_t* p = &pixels_[(static_cast<size_t>(y) * kWidth + x) * 4];
        p[0] = rgb[0];
        p[1] = rgb[1];
        p[2] = rgb[2];
        p[3] = 255;
    }

    std::vector<uint8_t> pixels_;
    int cursorX_ = 0, cursorY_ = 0, rowHeight_ = 0, height_ = 0;
};

}  // namespace

std::optional<WorldGeometry> loadWorldGeometry(const std::string& path) {
    bsppp::BSP file(path);
    if (!file) {
        std::fprintf(stderr, "failed to open bsp: %s\n", path.c_str());
        return std::nullopt;
    }

    auto vertexes = file.getLumpData<bsppp::BSPLump::VERTEXES>();
    auto edges = file.getLumpData<bsppp::BSPLump::EDGES>();
    auto surfEdges = file.getLumpData<bsppp::BSPLump::SURFEDGES>();
    auto faces = file.getLumpData<bsppp::BSPLump::FACES>();
    auto planes = file.getLumpData<bsppp::BSPLump::PLANES>();
    auto models = file.getLumpData<bsppp::BSPLump::MODELS>();
    auto texInfos = file.getLumpData<bsppp::BSPLump::TEXINFO>();
    auto texDatas = file.getLumpData<bsppp::BSPLump::TEXDATA>();
    auto texNames = parseTexdataStringTable(file);

    if (vertexes.empty() || edges.empty() || surfEdges.empty() || faces.empty() ||
        planes.empty() || models.empty()) {
        std::fprintf(stderr, "bsp is missing required lumps: %s\n", path.c_str());
        return std::nullopt;
    }

    std::unordered_map<std::string, size_t> groupIndexByMaterial;
    WorldGeometry world;

    // vrad's output; LDR when the map has it, else the HDR copy (same format).
    auto lighting = file.getLumpData(bsppp::BSPLump::LIGHTING);
    if (!lighting || lighting->empty()) lighting = file.getLumpData(bsppp::BSPLump::LIGHTING_HDR);
    bool hasLighting = lighting && !lighting->empty();
    LightmapAtlas atlas;

    const auto& worldspawn = models[0];
    std::vector<glm::vec3> loop;

    for (int32_t faceIdx = worldspawn.firstFace;
         faceIdx < worldspawn.firstFace + worldspawn.numFaces;
         ++faceIdx) {
        const auto& face = faces[faceIdx];
        if (face.numEdges < 3) {
            continue;
        }
        if (face.planeNum >= planes.size()) {
            continue;
        }

        // faces the engine never draws: sky (the skybox shows through them) and compile only tool faces that survived into the bsp
        constexpr int32_t kSurfSky2D = 0x2, kSurfSky = 0x4, kSurfNoDraw = 0x80, kSurfHint = 0x100, kSurfSkip = 0x200;
        if (face.texInfo >= 0 && static_cast<size_t>(face.texInfo) < texInfos.size() &&
            (texInfos[face.texInfo].flags & (kSurfSky2D | kSurfSky | kSurfNoDraw | kSurfHint | kSurfSkip)) != 0) {
            continue;
        }

        glm::vec3 normal = toGlm(planes[face.planeNum].normal);
        if (face.side) {
            normal = -normal;
        }

        // resolve this face's material + uv basis. faces with no valid texinfo (texInfo < 0, e.g. some skip/trigger adjacent faces) fall into a shared "" group rendered with the placeholder texture
        std::string materialName;
        glm::vec4 texVec1(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec4 texVec2(0.0f, 1.0f, 0.0f, 0.0f);
        float texWidth = 512.0f;
        float texHeight = 512.0f;
        if (face.texInfo >= 0 && static_cast<size_t>(face.texInfo) < texInfos.size()) {
            const auto& texInfo = texInfos[face.texInfo];
            texVec1 = toGlm(texInfo.textureVector1);
            texVec2 = toGlm(texInfo.textureVector2);
            if (texInfo.textureData >= 0 && static_cast<size_t>(texInfo.textureData) < texDatas.size()) {
                const auto& texData = texDatas[texInfo.textureData];
                texWidth = static_cast<float>(texData.width > 0 ? texData.width : 512);
                texHeight = static_cast<float>(texData.height > 0 ? texData.height : 512);
                if (texData.nameStringTableID >= 0 &&
                    static_cast<size_t>(texData.nameStringTableID) < texNames.size()) {
                    materialName = texNames[texData.nameStringTableID];
                }
            }
        }

        auto groupIt = groupIndexByMaterial.find(materialName);
        size_t groupIndex;
        if (groupIt == groupIndexByMaterial.end()) {
            groupIndex = world.groups.size();
            world.groups.push_back(MaterialGroup{materialName, {}});
            groupIndexByMaterial.emplace(materialName, groupIndex);
        } else {
            groupIndex = groupIt->second;
        }

        loop.clear();
        loop.reserve(face.numEdges);
        for (int32_t i = 0; i < face.numEdges; ++i) {
            int32_t se = surfEdges[face.firstEdge + i].surfEdge;
            const auto& edge = edges[se >= 0 ? se : -se];
            uint32_t vertIdx = se >= 0 ? edge.v0 : edge.v1;
            loop.push_back(toGlm(vertexes[vertIdx].position));
        }

        // this face's lightmap, if vrad gave it one: luxel (s, t) of the face comes from the lightmap vectors, offset by the face's mins
        glm::vec2 lightmapOrigin(-1.0f);
        glm::vec4 lmVec1(0.0f), lmVec2(0.0f);
        glm::vec2 lmMins(0.0f);
        int lmW = face.lightmapTextureSizeInLuxels[0] + 1, lmH = face.lightmapTextureSizeInLuxels[1] + 1;
        if (hasLighting && face.lightOffset >= 0 && face.styles[0] != 255 && face.texInfo >= 0 &&
            static_cast<size_t>(face.texInfo) < texInfos.size() && lmW > 0 && lmH > 0 &&
            static_cast<size_t>(face.lightOffset) + static_cast<size_t>(lmW) * lmH * 4 <= lighting->size()) {
            const auto& ti = texInfos[face.texInfo];
            lmVec1 = toGlm(ti.lightmapVector1);
            lmVec2 = toGlm(ti.lightmapVector2);
            lmMins = glm::vec2(face.lightmapTextureMinsInLuxels[0], face.lightmapTextureMinsInLuxels[1]);
            const auto* luxels = reinterpret_cast<const uint8_t*>(lighting->data()) + face.lightOffset;
            lightmapOrigin = glm::vec2(atlas.add(luxels, lmW, lmH));
        }

        auto makeVertex = [&](const glm::vec3& pos) {
            glm::vec2 uv(
                (glm::dot(pos, glm::vec3(texVec1)) + texVec1.w) / texWidth,
                (glm::dot(pos, glm::vec3(texVec2)) + texVec2.w) / texHeight);
            // atlas pixel coords for now, normalized once the atlas size is final
            glm::vec2 lm = atlas.whiteTexel();
            if (lightmapOrigin.x >= 0.0f) {
                glm::vec2 luxel(glm::dot(pos, glm::vec3(lmVec1)) + lmVec1.w - lmMins.x,
                                glm::dot(pos, glm::vec3(lmVec2)) + lmVec2.w - lmMins.y);
                lm = lightmapOrigin + luxel + glm::vec2(0.5f);
            }
            return Vertex{pos, normal, uv, lm};
        };

        // fan triangulation, faces in a bsp are convex polygons
        auto& triangles = world.groups[groupIndex].triangles;
        for (size_t i = 1; i + 1 < loop.size(); ++i) {
            triangles.push_back(makeVertex(loop[0]));
            triangles.push_back(makeVertex(loop[i]));
            triangles.push_back(makeVertex(loop[i + 1]));
        }
    }

    if (hasLighting) {
        glm::vec2 size(LightmapAtlas::kWidth, atlas.height());
        for (auto& group : world.groups)
            for (auto& v : group.triangles) v.lightmapUv /= size;
        world.lightmapAtlas = atlas.finish();
        std::printf("lightmaps: %dx%d atlas\n", world.lightmapAtlas.width, world.lightmapAtlas.height);
    } else {
        std::printf("lightmaps: none in this map (unlit compile)\n");
    }

    size_t totalTriangles = 0;
    for (const auto& group : world.groups) {
        totalTriangles += group.triangles.size() / 3;
    }
    std::printf("loaded %zu triangles across %zu material groups from worldspawn (%s)\n", totalTriangles,
                world.groups.size(), path.c_str());
    return world;
}

}  // namespace bsp
