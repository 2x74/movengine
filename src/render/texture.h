#pragma once

#include <glad/gl.h>

#include "bsp/material_loader.h"

namespace render {

class Texture {
public:
    // mipmaps = false for lightmap atlases: mip levels would average
    // neighboring faces' lightmaps together.
    void upload(const bsp::DecodedTexture& decoded, bool mipmaps = true);
    void destroy();
    void bind(GLuint unit = 0) const;
    GLuint id() const { return id_; }

private:
    GLuint id_ = 0;
};

}  // namespace render
