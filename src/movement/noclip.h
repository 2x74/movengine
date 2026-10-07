#pragma once

#include <glm/glm.hpp>

namespace movement {

struct NoclipInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;  // included so holding forward while looking up/down actually climbs/dives
};

struct NoclipState {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
};

// source's real noclip isn't an instant teleport to wish velocity like the freefly cam, it runs the same wishdir/accelerate math as ground movement in full 3d with no gravity or collision, so it has momentum (keeps drifting when you let go). the real sv_noclipspeed is a multiplier on maxSpeed (default ~5, which is why infinite's 11.400651 looks odd), but this takes units so changing sv_maxspeed can't change how fast noclip flies
void tickNoclip(NoclipState& state, const NoclipInput& input, float noclipMaxSpeed, float accelerate,
                 float friction, float dt);

}  // namespace movement
