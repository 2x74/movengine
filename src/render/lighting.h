#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

#include "bsp/world_geometry.h"

namespace render {

// uniform locations for kTexturedFragmentSrc's lighting inputs
struct LightingUniforms {
    GLint mode = -1, lightmap = -1, alpha = -1;
    GLint sunDirection = -1, sunColor = -1, ambient = -1;
    GLint pointCount = -1, pointPos = -1, pointColor = -1, pointFifty = -1;

    void locate(GLuint program);

    // sets everything for one frame. mode 1 binds `lightmapTexture` to unit 1, mode 2 uploads the lights nearest `viewPos` (the shader takes 32)
    void apply(int mode, GLuint lightmapTexture, const bsp::SceneLighting& lighting, const glm::vec3& viewPos,
               float alpha = 1.0f) const;
};

}  // namespace render
