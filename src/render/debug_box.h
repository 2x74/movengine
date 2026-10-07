#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

namespace render {

// a reusable unit cube (-0.5..0.5 on each axis), drawn scaled+translated to cover an arbitrary aabb. used to render zone volumes in world so you can actually see where a zone is, not just read its coordinates in a console
class DebugBox {
public:
    void init();
    void destroy();
    // caller must already have the debug box shader bound (glUseProgram) and uViewProj set, this computes+uploads uModel for [min, max] and draws
    void draw(GLint modelUniformLoc, const glm::vec3& min, const glm::vec3& max) const;

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
};

}  // namespace render
