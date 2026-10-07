#include "render/shader.h"

#include <cstdio>
#include <vector>

namespace render {

namespace {

GLuint compile(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint logLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(static_cast<size_t>(logLen) + 1);
        glGetShaderInfoLog(shader, logLen, nullptr, log.data());
        std::fprintf(stderr, "shader compile failed: %s\n", log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

GLuint createShaderProgram(const char* vertexSrc, const char* fragmentSrc) {
    GLuint vs = compile(GL_VERTEX_SHADER, vertexSrc);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragmentSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint logLen = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(static_cast<size_t>(logLen) + 1);
        glGetProgramInfoLog(program, logLen, nullptr, log.data());
        std::fprintf(stderr, "shader link failed: %s\n", log.data());
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

}  // namespace render
