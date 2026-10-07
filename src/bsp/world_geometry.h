#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "bsp/material_loader.h"

namespace bsp {

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec2 lightmapUv{0.0f};  // into WorldGeometry::lightmapAtlas, when there is one
};

// all triangles using the same material as a single non indexed list, one draw call and one texture bind per group
struct MaterialGroup {
    std::string materialName;  // e.g. "CONCRETE/CINDERWALL03B_CHEAP", as stored in TEXDATA_STRING_DATA
    std::vector<Vertex> triangles;
};

// worldspawn model (model 0) only, grouped by material. props and non worldspawn brush models are still out of scope
struct WorldGeometry {
    std::vector<MaterialGroup> groups;
    // the map's baked (vrad) lighting, every face's lightmap packed into one texture. empty when the map has none (unlit compile, or a .vmf)
    DecodedTexture lightmapAtlas;
};

// live lighting for maps without baked lightmaps (a .vmf being playtested), read from its light entities. no shadows, that's what compiling is for
struct PointLight {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};          // linear RGB, already scaled by brightness
    float fiftyPercentDistance = 100.0f;
};

struct SceneLighting {
    bool hasLights = false;          // any light entity at all; otherwise the plain default shading
    bool hasSun = false;
    glm::vec3 sunDirection{0.0f, 0.0f, -1.0f};  // the way sunlight travels
    glm::vec3 sunColor{0.0f};
    glm::vec3 ambient{0.0f};
    std::vector<PointLight> points;
};

// a map's env_fog_controller, as the shader wants it
struct SceneFog {
    bool enabled = false;
    glm::vec3 color{0.6f, 0.65f, 0.72f};
    float start = 0.0f;
    float end = 0.0f;
    float maxDensity = 1.0f;
};

}  // namespace bsp
