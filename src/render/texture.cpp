#include "render/texture.h"

namespace render {

namespace {

void uploadCommon(int width, int height, const void* pixels, bool mipmaps) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
    GLint wrap = mipmaps ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

}  // namespace

void Texture::upload(const bsp::DecodedTexture& decoded, bool mipmaps) {
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);
    uploadCommon(decoded.width, decoded.height, decoded.rgba8888.data(), mipmaps);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::destroy() {
    if (id_) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
}

void Texture::bind(GLuint unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, id_);
}

}  // namespace render
