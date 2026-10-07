#include "movement/noclip.h"

#include <cmath>

namespace movement {

namespace {
glm::vec3 forward3D(float yawDeg, float pitchDeg) {
    float yaw = glm::radians(yawDeg);
    float pitch = glm::radians(pitchDeg);
    return glm::normalize(glm::vec3(std::cos(pitch) * std::cos(yaw),
                                     std::cos(pitch) * std::sin(yaw),
                                     std::sin(pitch)));
}
glm::vec3 right3D(float yawDeg) {
    float yaw = glm::radians(yawDeg);
    return {std::sin(yaw), -std::cos(yaw), 0.0f};
}
}  // namespace

void tickNoclip(NoclipState& state, const NoclipInput& input, float noclipMaxSpeed, float accelerate,
                 float friction, float dt) {
    glm::vec3 forward = forward3D(input.yawDeg, input.pitchDeg);
    glm::vec3 right = right3D(input.yawDeg);
    constexpr glm::vec3 kUp(0.0f, 0.0f, 1.0f);

    glm::vec3 wishDir(0.0f);
    if (input.forward) wishDir += forward;
    if (input.back) wishDir -= forward;
    if (input.right) wishDir += right;
    if (input.left) wishDir -= right;
    if (input.up) wishDir += kUp;
    if (input.down) wishDir -= kUp;
    if (glm::length(wishDir) > 0.0001f) {
        wishDir = glm::normalize(wishDir);
    }

    // friction (always on decel when you're not adding speed) is what gives it momentum instead of an instant stop
    float speed = glm::length(state.velocity);
    if (speed > 0.1f) {
        float drop = speed * friction * dt;
        float scale = std::max(speed - drop, 0.0f) / speed;
        state.velocity *= scale;
    } else {
        state.velocity = glm::vec3(0.0f);
    }

    float currentSpeed = glm::dot(state.velocity, wishDir);
    float addSpeed = noclipMaxSpeed - currentSpeed;
    if (addSpeed > 0.0f) {
        float accelSpeed = std::min(accelerate * noclipMaxSpeed * dt, addSpeed);
        state.velocity += wishDir * accelSpeed;
    }

    state.position += state.velocity * dt;
}

}  // namespace movement
