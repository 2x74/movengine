#include "render/camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace render {

namespace {
constexpr glm::vec3 kWorldUp(0.0f, 0.0f, 1.0f);
}

void FreeFlyCamera::look(float dxPixels, float dyPixels) {
    // forwardVector()'s right hand triple (forward, worldUp) puts "right" at negative yaw, so mouse right (positive dx) must decrease yaw to turn the view right instead of left
    // source's formula: degrees = counts * sensitivity * m_yaw, with m_yaw 0.022. without the m_yaw factor the same sensitivity number turns ~45x further here than in css, so a sensitivity carried over from there is unusable and strafe timing built on it doesn't transfer
    yawDeg_ -= dxPixels * mouseSensitivity_ * mYaw_;
    pitchDeg_ -= dyPixels * mouseSensitivity_ * mPitch_;
    pitchDeg_ = glm::clamp(pitchDeg_, -89.0f, 89.0f);
}

glm::vec3 FreeFlyCamera::forwardVector() const {
    float yaw = glm::radians(yawDeg_);
    float pitch = glm::radians(pitchDeg_);
    return glm::normalize(glm::vec3(
        std::cos(pitch) * std::cos(yaw),
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch)));
}

void FreeFlyCamera::update(const MoveInput& input, float dtSeconds) {
    glm::vec3 forward = forwardVector();
    glm::vec3 right = glm::normalize(glm::cross(forward, kWorldUp));

    glm::vec3 move(0.0f);
    if (input.forward) move += forward;
    if (input.back) move -= forward;
    if (input.right) move += right;
    if (input.left) move -= right;
    if (input.up) move += kWorldUp;
    if (input.down) move -= kWorldUp;

    if (glm::length(move) > 0.0f) {
        position += glm::normalize(move) * moveSpeed_ * dtSeconds;
    }
}

glm::mat4 FreeFlyCamera::viewMatrix() const {
    return glm::lookAt(position, position + forwardVector(), kWorldUp);
}

glm::mat4 FreeFlyCamera::projectionMatrix(float aspect, float nearPlane, float farPlane) const {
    return glm::perspective(glm::radians(fovDeg_), aspect, nearPlane, farPlane);
}

}  // namespace render
