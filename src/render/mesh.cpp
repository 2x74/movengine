#include "render/mesh.h"

namespace render {

void Mesh::upload(const std::vector<bsp::Vertex>& triangles) {
    vertexCount_ = static_cast<GLsizei>(triangles.size());

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(triangles.size() * sizeof(bsp::Vertex)),
                 triangles.data(),
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(bsp::Vertex),
                           reinterpret_cast<void*>(offsetof(bsp::Vertex, position)));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(bsp::Vertex),
                           reinterpret_cast<void*>(offsetof(bsp::Vertex, normal)));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(bsp::Vertex),
                           reinterpret_cast<void*>(offsetof(bsp::Vertex, uv)));

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(bsp::Vertex),
                           reinterpret_cast<void*>(offsetof(bsp::Vertex, lightmapUv)));

    glBindVertexArray(0);
}

void Mesh::destroy() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    vbo_ = 0;
    vao_ = 0;
    vertexCount_ = 0;
}

void Mesh::draw() const {
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount_);
    glBindVertexArray(0);
}

}  // namespace render
