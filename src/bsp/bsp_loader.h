#pragma once

#include <optional>
#include <string>

#include "bsp/world_geometry.h"

namespace bsp {

// loads the worldspawn model (model 0) of a .bsp into flat shaded render geometry. returns nullopt on any load/parse failure
std::optional<WorldGeometry> loadWorldGeometry(const std::string& path);

}  // namespace bsp
