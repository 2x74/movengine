#pragma once

#include <glad/gl.h>

namespace render {

// compiles+links a vertex/fragment pair. returns 0 and logs to stderr on failure
GLuint createShaderProgram(const char* vertexSrc, const char* fragmentSrc);

}  // namespace render
