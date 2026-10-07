#include "render/lighting.h"

#include <algorithm>
#include <vector>

#include "render/shaders.h"

namespace render {

void LightingUniforms::locate(GLuint program) {
    mode = glGetUniformLocation(program, "uLightingMode");
    lightmap = glGetUniformLocation(program, "uLightmap");
    alpha = glGetUniformLocation(program, "uAlpha");
    sunDirection = glGetUniformLocation(program, "uSunDirection");
    sunColor = glGetUniformLocation(program, "uSunColor");
    ambient = glGetUniformLocation(program, "uAmbient");
    pointCount = glGetUniformLocation(program, "uPointCount");
    pointPos = glGetUniformLocation(program, "uPointPos");
    pointColor = glGetUniformLocation(program, "uPointColor");
    pointFifty = glGetUniformLocation(program, "uPointFifty");
}

void LightingUniforms::apply(int lightingMode, GLuint lightmapTexture, const bsp::SceneLighting& lighting,
                             const glm::vec3& viewPos, float alphaValue) const {
    glUniform1i(mode, lightingMode);
    glUniform1f(alpha, alphaValue);
    glUniform1i(lightmap, 1);
    if (lightingMode == 1) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, lightmapTexture);
        glActiveTexture(GL_TEXTURE0);
    }
    if (lightingMode != 2) return;

    glUniform3fv(sunDirection, 1, &lighting.sunDirection.x);
    glm::vec3 sun = lighting.hasSun ? lighting.sunColor : glm::vec3(0.0f);
    glUniform3fv(sunColor, 1, &sun.x);
    glUniform3fv(ambient, 1, &lighting.ambient.x);

    // more lights than the shader takes: keep the ones nearest the camera
    std::vector<const bsp::PointLight*> nearest;
    for (const auto& p : lighting.points) nearest.push_back(&p);
    auto byDistance = [&](const bsp::PointLight* a, const bsp::PointLight* b) {
        return glm::dot(a->position - viewPos, a->position - viewPos) < glm::dot(b->position - viewPos, b->position - viewPos);
    };
    if (nearest.size() > static_cast<size_t>(kMaxPointLights)) {
        std::partial_sort(nearest.begin(), nearest.begin() + kMaxPointLights, nearest.end(), byDistance);
        nearest.resize(kMaxPointLights);
    }
    std::vector<glm::vec3> pos, color;
    std::vector<float> fifty;
    for (const auto* p : nearest) {
        pos.push_back(p->position);
        color.push_back(p->color);
        fifty.push_back(std::max(p->fiftyPercentDistance, 1.0f));
    }
    glUniform1i(pointCount, static_cast<GLint>(nearest.size()));
    if (!nearest.empty()) {
        glUniform3fv(pointPos, static_cast<GLsizei>(pos.size()), &pos[0].x);
        glUniform3fv(pointColor, static_cast<GLsizei>(color.size()), &color[0].x);
        glUniform1fv(pointFifty, static_cast<GLsizei>(fifty.size()), fifty.data());
    }
}

}  // namespace render
