#pragma once

#include <optional>
#include <string>

#include "vmf/document.h"

namespace vmf {

// a compiled map back into an editable document, like bspsource: brushes (world, func_detail, brush entities, with their textures and texture alignment), point entities and outputs. what compiling threw away stays lost: displacements come back as their flat base brush, and lights without a name were baked into the lightmaps and aren't entities any more. the source .bsp is recorded in worldspawn under kPakfileKey so the game and editor keep finding the textures packed inside it
std::optional<Document> importBsp(const std::string& path);

inline constexpr const char* kPakfileKey = "bhop_pakfile";

// vbsp renames materials it patches for cubemaps and water ("maps/bhop_x/concrete/wall_-128_256_64", "..._wvt_patch"), this gives back the material the mapper picked
std::string unpatchMaterialName(const std::string& name);

}  // namespace vmf
