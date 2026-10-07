#pragma once

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

#include "bsp/material_loader.h"

namespace content {

// textures generated in code so they ship with the app and work without css: hammer style dev/tool lookalikes (same material names as css, which a css install overrides with the real ones) plus a few plain surfaces of our own to build with
struct BuiltinTexture {
    bsp::DecodedTexture pixels;
    glm::vec2 uvSize{512.0f};  // texel size used for texture-axis math, like a VTF's width/height
};

// upper case material names, dev/tool first then our own BHOP/ set
const std::vector<std::string>& builtinMaterialNames();

// case insensitive, nullopt if it isn't one of the built-ins
std::optional<BuiltinTexture> builtinTexture(const std::string& material);

// what a material that can't be found anywhere renders as: a dev style grid stamped "MT" (missing texture) instead of source's purple/black checker
BuiltinTexture missingTexture();

}  // namespace content
