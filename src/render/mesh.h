#pragma once

#include <glad/gl.h>

#include "bsp/world_geometry.h"

namespace render {

// a gpu resident, non indexed triangle list, one material group's worth
class Mesh {
public:
    void upload(const std::vector<bsp::Vertex>& triangles);
    void destroy();
    void draw() const;

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLsizei vertexCount_ = 0;
};

}  // namespace render
