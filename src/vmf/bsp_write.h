#pragma once

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

#include "vmf/document.h"

namespace vmf {

// a material's texture size in pixels, needed for texinfo's texture axes. return {0, 0} for an unknown material and 512x512 is assumed
using TextureSizeLookup = std::function<glm::ivec2(const std::string&)>;

struct BspWriteStats {
    int planes = 0;
    int vertices = 0;
    int edges = 0;
    int faces = 0;
    int texInfos = 0;
    int materials = 0;
    int brushes = 0;
    int brushSides = 0;
    int models = 0;
    int entities = 0;
    int lightmapLuxels = 0;
    int skippedSolids = 0;  // hint/skip brushes, and solids with no interior
};

struct BspWriteResult {
    bool ok = false;
    std::string error;
    BspWriteStats stats;
};

// writes `doc` straight out as a source bsp (map version 20 with the per lump versions css maps carry), without valve's compilers.
//
// what it does produce: real world geometry with the map's own texture projections and lightmap axes, brush collision for the world and for every brush entity (each gets its own *N model), and the entity lump.
//
// what it does NOT produce, and why it's the fallback and not the default: no visibility tree. vvis's job is to work out which parts of a map can see which and there's no cheap stand in for it, so the whole map is one leaf in one cluster that always sees itself. correct, but the engine draws everything all the time. and no radiosity: vrad's job, replaced by a flat fullbright lightmap, so a map compiled this way has no baked shadows. prefer findCompileTools()/runValveCompile() whenever the tools are there
BspWriteResult writeBsp(const std::string& path, const Document& doc,
                        const TextureSizeLookup& textureSize = {});

}  // namespace vmf
