#pragma once

#include <glm/glm.hpp>

namespace render {

// debug only freefly camera for validating bsp geometry, z up to match source's coordinate convention (the real player camera arrives in the movement module, step 5 of the build order)
class FreeFlyCamera {
public:
    struct MoveInput {
        bool forward = false;
        bool back = false;
        bool left = false;
        bool right = false;
        bool up = false;
        bool down = false;
    };

    void look(float dxPixels, float dyPixels);
    void update(const MoveInput& input, float dtSeconds);

    glm::mat4 viewMatrix() const;
    // defaults reach across the biggest source maps (±16384 units each way)
    glm::mat4 projectionMatrix(float aspect, float nearPlane = 2.0f, float farPlane = 65536.0f) const;

    float yawDegrees() const { return yawDeg_; }
    float pitchDegrees() const { return pitchDeg_; }
    void setYawDegrees(float yaw) { yawDeg_ = yaw; }
    void setPitchDegrees(float pitch) { pitchDeg_ = glm::clamp(pitch, -89.0f, 89.0f); }
    // source pitch is positive down, ours is positive up
    void setSourcePitchDegrees(float sourcePitch) { pitchDeg_ = glm::clamp(-sourcePitch, -89.0f, 89.0f); }

    void setMouseSensitivity(float sensitivity) { mouseSensitivity_ = sensitivity; }
    float mouseSensitivity() const { return mouseSensitivity_; }
    // source's m_yaw/m_pitch: degrees turned per count per unit of sensitivity
    void setMouseYaw(float mYaw) { mYaw_ = mYaw; }
    void setMousePitch(float mPitch) { mPitch_ = mPitch; }
    float mouseYaw() const { return mYaw_; }
    // turn the view by this many degrees (turnbinds, +left is positive)
    void turn(float degrees) { yawDeg_ += degrees; }

    void setFovDegrees(float fov) { fovDeg_ = fov; }

    void setMoveSpeed(float unitsPerSecond) { moveSpeed_ = unitsPerSecond; }
    float moveSpeed() const { return moveSpeed_; }
    float fovDegrees() const { return fovDeg_; }

    glm::vec3 position = glm::vec3(0.0f, 0.0f, 64.0f);

private:
    glm::vec3 forwardVector() const;

    float yawDeg_ = 0.0f;
    float pitchDeg_ = 0.0f;
    float moveSpeed_ = 400.0f;  // source units/sec
    // css's own defaults so a sensitivity copied from there feels the same
    float mouseSensitivity_ = 2.5f;
    float mYaw_ = 0.022f;
    float mPitch_ = 0.022f;
    float fovDeg_ = 90.0f;  // source's classic default horizontal-ish fov
};

}  // namespace render
