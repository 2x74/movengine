#pragma once

#include <glm/glm.hpp>

#include "collision/world_brushes.h"
#include "movement/styles.h"

namespace movement {

struct MoveInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool jump = false;
    bool duck = false;    // ctrl, matches real CS:S's default +duck bind
    bool attack = false;  // left mouse, only test2's auto-strafe uses this so far
    float yawDeg = 0.0f;   // view yaw, used to compute the wish direction and as test2's steering base
};

// defaults pulled from the real server (angelgirl.cloud, Oct 2026) not guessed: sv_airaccelerate/sv_enablebunnyhopping from cstrike/cfg/server.cfg, sv_accelerate/sv_stopspeed from cstrike/cfg/skill1.cfg, airAccelerate re-confirmed per style as "Normal"'s value in shavit-styles.cfg. sv_friction/sv_gravity/sv_maxspeed weren't overridden on the server so those are still source's engine defaults (4, 800) not server verified. ground speed is 260 not sv_maxspeed's 320 cause in css your weapon caps it (260 scout, 250 knife) and 260 is what this plays at by default
// airAccelerate's 30 u/s wishspeed cap is source's real engine constant, confirmed from test2dll.sp
struct MoveConstants {
    float maxSpeed = 260.0f;
    float accelerate = 5.0f;
    float airAccelerate = 1000.0f;
    float airSpeedCap = 30.0f;
    float friction = 4.0f;
    float stopSpeed = 75.0f;
    float gravity = 800.0f;
    float jumpVelocity = 301.993377f;  // CS:S: sqrt(2 * 800 * 57), a 57-unit jump
    float playerHalfWidth = 16.0f;
    float playerHalfHeight = 36.0f;  // standing hull is 72 units tall
    float duckHalfHeight = 27.0f;    // ducked hull is 54 units tall, like CS:S
    // source's real ducked ground speed is a flat ~85 u/s not a maxspeed fraction, but that isn't server verified like the other constants here (no sv_ cvar for it, it's compiled into the engine). 0.34 approximates it off the 260 default and is tunable with the duckspeed cvar rather than claimed exact
    float duckSpeedMultiplier = 0.34f;
};

struct PlayerState {
    glm::vec3 position{0.0f};  // center of the AABB, not the feet
    glm::vec3 velocity{0.0f};
    bool grounded = false;
    bool ducking = false;  // current actual hull state not the input, can lag release if something's overhead

    // style specific persistent state, reset on restart (see resetRunState)
    int lockedKeyCombo = -1;     // a-only / surf-half-sideways: which combo the run committed to
    bool jumpHeldLastTick = false;
    int jumpCooldownTicks = 0;   // infinite's midair jump, matches infinite.sp's 3-tick cooldown

    // test2's auto-strafe output: main.cpp applies this to the camera after
    // the tick (movement doesn't own the camera, so it can't set it directly).
    bool hasYawOverride = false;
    float yawOverrideDeg = 0.0f;

    // source's base velocity: trigger_push sets it every tick you're inside (baseVelocityActive = FL_BASEVELOCITY) and it carries you along without being part of your own velocity. the tick nothing renews it, it's folded into velocity as momentum, which is also how "AddOutput basevelocity" boosters work since those set it without the flag. written by movement::tickTriggers, consumed by tickPlayerMove
    glm::vec3 baseVelocity{0.0f};
    bool baseVelocityActive = false;

    // for the trigger code (rngfix style fixes)
    float groundGap = 0.0f;  // grounded: how far below the hull the ground is (0-2 units)
    glm::vec3 preCollisionVelocity{0.0f};  // this tick's velocity before anything was hit
    bool collidedThisTick = false;         // hit a wall/floor or landed this tick

    // timer stat outputs for this tick, consumed by movement::tickTimer
    bool justJumped = false;
    bool speedGainedInAir = false;  // this tick's air-accelerate actually added speed (sync stat)
    float airGain = 0.0f;           // airborne ticks: horizontal speed air-accelerate added this tick
    float airGainMax = 0.0f;        // ...and the most it could have added with perfect strafing
};

// clears the per run state above (lockedKeyCombo, jump cooldown) without touching position/velocity. call this alongside a position reset so a restart doesn't carry over e.g. a surf hsw combo lock from the last run
void resetRunState(PlayerState& state);

// runs one fixed timestep physics tick against the world brushes: style input gating, ground probe, jump (incl. infinite's midair override and legit's no autobhop), friction, ground/air accelerate (the bhop defining asymmetry, with the style's airaccelerate override and gravity multiplier), test2's auto strafe, gravity, move + collide. not yet the full plan pipeline (no multi pass slide, no step offset, no split gravity), this is the minimum real collision bound movement to replace the freefly debug camera, refine from here
void tickPlayerMove(PlayerState& state, MoveInput input, const MoveConstants& constants,
                     const StyleDef& style, const collision::WorldBrushes& world, float dt);

}  // namespace movement
