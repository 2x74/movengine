#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "bsp/material_loader.h"
#include "content/game_content.h"

namespace content {

// reads a file by game relative path ("materials/x.vmt"), or nullopt
using FileReader = std::function<std::optional<std::vector<std::byte>>(const std::string& path)>;

struct ResolvedMaterial {
    enum class Source { Map, Game, Builtin, Missing };
    bsp::DecodedTexture pixels;
    glm::vec2 uvSize{512.0f};  // the texture's size in texels, for texture-axis UV math
    Source source = Source::Missing;
    // $alphatest: a hard cutout. foliage, fences and grates are a flat quad with the shape in the alpha channel so without this they draw as the whole rectangle
    bool alphaTest = false;
    float alphaTestRef = 0.5f;  // $alphatestreference
    // $translucent: blended rather than cut out (glass, some water).
    bool translucent = false;
};

// lookup order: the map's own packed files (a .bsp's pakfile), the mounted css install, the built-in textures, and finally the "MT" texture
ResolvedMaterial resolveMaterial(const std::string& material, const GameContent* game,
                                 const FileReader& mapPak = nullptr);

// reader over a .bsp's embedded pakfile (opened once, shared by the copies)
FileReader bspPakReader(const std::string& bspPath);

}  // namespace content
