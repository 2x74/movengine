#include "movement/player_move.h"

#include "collision/clip.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace movement {

namespace {

// matches render::FreeFlyCamera's convention: right = cross(forward, worldUp) with forward = (cos(yaw), sin(yaw), 0), which works out to (sin(yaw), -cos(yaw), 0)
glm::vec3 forwardFromYaw(float yawDeg) {
    float yawRad = glm::radians(yawDeg);
    return {std::cos(yawRad), std::sin(yawRad), 0.0f};
}
glm::vec3 rightFromYaw(float yawDeg) {
    float yawRad = glm::radians(yawDeg);
    return {std::sin(yawRad), -std::cos(yawRad), 0.0f};
}

// ported from shavit-core.sp's real block_w/a/s/d, a_or_d_only and force_hsw (1 and 2), not a reinvention. source cancels opposite held keys at the command level (w+s nets to nothing) so the fwd/back/left/right locals below do that before any style rule sees them, same as shavit's vel[0]/vel[1] checks
void applyStyleGating(const StyleDef& style, MoveInput& input, int& lockedKeyCombo) {
    bool fwd = input.forward && !input.back;
    bool back = input.back && !input.forward;
    bool left = input.left && !input.right;
    bool right = input.right && !input.left;

    if (style.blockW) fwd = false;
    if (style.blockS) back = false;
    if (style.blockA) left = false;
    if (style.blockD) right = false;

    if (style.aOrDOnly) {
        int combo = -1;
        if (left) combo = 0;
        else if (right) combo = 1;
        if (combo != -1) {
            if (lockedKeyCombo == -1) {
                lockedKeyCombo = combo;
            }
            if (combo != lockedKeyCombo) {
                left = false;
                right = false;
            }
        }
    }

    if (style.forceHsw == 1) {
        // only w+strafe (either side) moves you, s+strafe, w alone and strafe alone all get zeroed. this is "holding w while doing a/d"
        if (back && (left || right)) {
            fwd = false;
            back = false;
        }
        if (fwd && !(left || right)) {
            fwd = false;
            back = false;
        }
        if ((left || right) && !fwd) {
            left = false;
            right = false;
        }
    } else if (style.forceHsw == 2) {
        // surf steering: the first valid combo (w+a or s+d) vs (w+d or s+a) locks in for the whole run, anything else gets zeroed
        int combo = -1;
        if ((fwd && left) || (back && right)) {
            combo = 0;
        } else if ((fwd && right) || (back && left)) {
            combo = 1;
        }
        if (lockedKeyCombo == -1 && combo != -1) {
            lockedKeyCombo = combo;
        }
        if ((lockedKeyCombo == 0 && combo != 0) || (lockedKeyCombo == 1 && combo != 1) ||
            (lockedKeyCombo == -1 && combo == -1)) {
            fwd = false;
            back = false;
            left = false;
            right = false;
        }
    }

    input.forward = fwd;
    input.back = back;
    input.left = left;
    input.right = right;
}

constexpr float kStepSize = 18.0f;  // sv_stepsize: tallest ledge you walk up without jumping

glm::vec3 clipVelocity(const glm::vec3& v, const glm::vec3& normal) {
    float into = glm::dot(v, normal);
    return into < 0.0f ? v - normal * into : v;
}

struct SlideResult {
    glm::vec3 pos;
    glm::vec3 vel;
    bool hitFloor = false;  // touched something walkable (normal.z > 0.7)
    bool blocked = false;   // touched anything that isn't a floor
    // airborne moves: the first touchdown on walkable ground, where it happened and the horizontal velocity coming in
    bool landed = false;
    bool stoppedAtLanding = false;
    glm::vec3 landPos{0.0f};
    glm::vec3 landVel{0.0f};
};

// TryPlayerMove: sweep the hull along vel for dt, stop at the first surface, slide along it and keep going with the time left, up to 4 times. velocity gets clipped against every surface hit since the last good move so corners slide along the crease instead of jittering, and a move that would turn back on itself just stops
SlideResult slideMove(const collision::WorldBrushes& world, const glm::vec3& start, const glm::vec3& vel,
                      const glm::vec3& half, float dt, bool airborne = false) {
    SlideResult r{start, vel};
    std::array<glm::vec3, 5> planes;
    int numPlanes = 0;
    glm::vec3 primal = vel;
    glm::vec3 original = vel;
    float timeLeft = dt;
    for (int bump = 0; bump < 4; ++bump) {
        if (glm::dot(r.vel, r.vel) < 1e-8f) break;
        collision::TraceResult tr = collision::traceBox(world, r.pos, r.pos + r.vel * timeLeft, half);
        if (tr.allSolid) {
            r.vel = glm::vec3(0.0f);
            break;
        }
        if (tr.fraction > 0.0f) {
            r.pos = tr.endPos;
            original = r.vel;
            numPlanes = 0;
        }
        if (tr.fraction >= 1.0f) break;

        if (tr.normal.z > 0.7f) r.hitFloor = true;
        else r.blocked = true;

        // touching down (walkable, not rising past 140 u/s so it counts as landing). rngfix's uphill fix: if deflecting off this would cost horizontal speed (jumping up an incline) land on it without the deflection, like when the tick happens to end just above it. whether you lost speed used to be luck
        if (airborne && !r.landed && tr.normal.z > 0.7f && r.vel.z <= 140.0f) {
            r.landed = true;
            r.landPos = r.pos;
            r.landVel = glm::vec3(r.vel.x, r.vel.y, 0.0f);
            glm::vec3 deflected = clipVelocity(r.vel, tr.normal);
            if (glm::length(glm::vec2(deflected)) < glm::length(glm::vec2(r.vel)) - 1e-3f) {
                r.vel = r.landVel;
                r.stoppedAtLanding = true;
                break;
            }
        }
        timeLeft -= timeLeft * tr.fraction;
        if (numPlanes >= static_cast<int>(planes.size())) {
            r.vel = glm::vec3(0.0f);
            break;
        }
        planes[numPlanes++] = tr.normal;

        int i = 0;
        for (; i < numPlanes; ++i) {
            glm::vec3 candidate = clipVelocity(original, planes[i]);
            int j = 0;
            for (; j < numPlanes; ++j) {
                if (j != i && glm::dot(candidate, planes[j]) < 0.0f) break;
            }
            if (j == numPlanes) {
                r.vel = candidate;
                break;
            }
        }
        if (i == numPlanes) {
            if (numPlanes != 2) {
                r.vel = glm::vec3(0.0f);
                break;
            }
            glm::vec3 crease = glm::cross(planes[0], planes[1]);
            float len = glm::length(crease);
            r.vel = len > 1e-6f ? crease * (glm::dot(crease, r.vel) / (len * len)) : glm::vec3(0.0f);
        }
        if (glm::dot(r.vel, primal) <= 0.0f) {
            r.vel = glm::vec3(0.0f);
            break;
        }
    }
    return r;
}

// StepMove: when walking into something also try the move lifted by a step and set back down, keep whichever got further. that's how you walk up stairs and small ledges instead of snagging
SlideResult walkMove(const collision::WorldBrushes& world, const glm::vec3& start, const glm::vec3& vel,
                     const glm::vec3& half, float dt) {
    SlideResult down = slideMove(world, start, vel, half, dt);
    if (!down.blocked) return down;

    collision::TraceResult up = collision::traceBox(world, start, start + glm::vec3(0, 0, kStepSize), half);
    if (up.allSolid) return down;
    SlideResult stepped = slideMove(world, up.endPos, vel, half, dt);
    collision::TraceResult land =
        collision::traceBox(world, stepped.pos, stepped.pos - glm::vec3(0, 0, kStepSize), half);
    if (land.allSolid || land.fraction >= 1.0f || land.normal.z < 0.7f) return down;
    stepped.pos = land.endPos;

    auto dist2 = [&](const glm::vec3& p) {
        glm::vec2 d(p.x - start.x, p.y - start.y);
        return glm::dot(d, d);
    };
    if (dist2(down.pos) >= dist2(stepped.pos)) return down;
    stepped.vel.z = down.vel.z;
    stepped.hitFloor = true;
    return stepped;
}

}  // namespace

void resetRunState(PlayerState& state) {
    state.lockedKeyCombo = -1;
    state.jumpHeldLastTick = false;
    state.jumpCooldownTicks = 0;
    state.hasYawOverride = false;
    state.ducking = false;
    state.baseVelocity = glm::vec3(0.0f);
    state.baseVelocityActive = false;
}

void tickPlayerMove(PlayerState& state, MoveInput input, const MoveConstants& c, const StyleDef& style,
                     const collision::WorldBrushes& world, float dt) {
    applyStyleGating(style, input, state.lockedKeyCombo);

    // CBasePlayer's momentum handoff: base velocity nothing renewed last tick (left the push, or a one shot AddOutput booster) becomes real velocity, with source's extra half frame of it
    if (!state.baseVelocityActive) {
        state.velocity += state.baseVelocity * (1.0f + dt * 0.5f);
        state.baseVelocity = glm::vec3(0.0f);
    }
    state.baseVelocityActive = false;

    // duck transition. on the ground the feet stay planted and the hull shrinks/grows at the top. in the air it shrinks around its middle so ducking pulls your feet up by half the height difference (9 units), that's the crouch jump reaching 64 unit ledges like css. crouching is always allowed, standing back up only commits if there's room (air: around the middle, else feet planted), otherwise it retries next tick like source auto standing when a low ceiling clears
    if (input.duck && !state.ducking) {
        if (state.grounded) {
            float feetZ = state.position.z - c.playerHalfHeight;
            state.position.z = feetZ + c.duckHalfHeight;
        }
        state.ducking = true;
    } else if (!input.duck && state.ducking) {
        glm::vec3 standHalfExtents(c.playerHalfWidth, c.playerHalfWidth, c.playerHalfHeight);
        float feetZ = state.position.z - c.duckHalfHeight;
        glm::vec3 planted(state.position.x, state.position.y, feetZ + c.playerHalfHeight);
        auto fits = [&](const glm::vec3& center) {
            return !collision::traceBox(world, center, center, standHalfExtents).startSolid;
        };
        if (!state.grounded && fits(state.position)) {
            state.ducking = false;
        } else if (fits(planted)) {
            state.position = planted;
            state.ducking = false;
        }
    }
    float currentHalfHeight = state.ducking ? c.duckHalfHeight : c.playerHalfHeight;
    glm::vec3 halfExtents(c.playerHalfWidth, c.playerHalfWidth, currentHalfHeight);

    // swept moves need a clear start. starting inside something (leaving noclip in a wall, spawning in a seam) gets pushed out first
    if (collision::traceBox(world, state.position, state.position, halfExtents).startSolid) {
        collision::resolveAabbVsBrushes(world, state.position, halfExtents);
    }

    // CategorizePosition: on the ground if a short trace down hits something walkable
    {
        bool wasGrounded = state.grounded;
        collision::TraceResult tr =
            collision::traceBox(world, state.position, state.position - glm::vec3(0.0f, 0.0f, 2.0f), halfExtents);
        bool hit = !tr.startSolid && tr.fraction < 1.0f;
        glm::vec3 normal = tr.normal;
        // moving up faster than NON_JUMP_VELOCITY (140) is never on ground, so boosters and push launches leave the floor
        state.grounded = hit && normal.z > 0.7f && state.velocity.z <= 140.0f;
        // landing by this 2 unit snap instead of actually hitting the slope would just drop the fall speed (zeroed below), so whether a slope boosted you came down to where the last tick happened to end. rngfix's downhill fix: deflect like a real hit whenever that gains horizontal speed (costs speed going uphill, where landing clean is better)
        if (state.grounded && !wasGrounded) {
            glm::vec3 deflected = clipVelocity(state.velocity, normal);
            if (glm::length(glm::vec2(deflected)) > glm::length(glm::vec2(state.velocity))) {
                state.velocity = deflected;
            }
        }
    }

    // FullWalkMove zeroes vertical speed on the ground (so a landing's along slope downward speed doesn't carry into walking). skipped while a push is lifting you so upward pushes can still build up and launch
    if (state.grounded && state.baseVelocity.z <= 0.0f) {
        state.velocity.z = 0.0f;
    }

    // StartGravity: vertical base velocity (a trigger_push pointing up) is applied as an acceleration and used up every tick, which is why vertical pushes in source depend on tickrate
    state.velocity.z += state.baseVelocity.z * dt;
    state.baseVelocity.z = 0.0f;

    // autobhop off (legit/scroll) needs a fresh press each hop, holding jump does nothing till you release and press again. matches shavit-core only clearing the already jumped state when the style's autobhop is on
    bool jumpPressed = input.jump && !state.jumpHeldLastTick;
    state.jumpHeldLastTick = input.jump;
    bool jumpTrigger = style.autoBhop ? input.jump : jumpPressed;

    state.justJumped = false;
    if (jumpTrigger && state.grounded) {
        // landfix: where the landing tick ends leaves you 0 to 2 units above the ground and you jump from there, so jump height varied hop to hop. normalize the takeoff height
        collision::TraceResult below =
            collision::traceBox(world, state.position, state.position - glm::vec3(0.0f, 0.0f, 2.0f), halfExtents);
        if (!below.startSolid && below.fraction < 1.0f) {
            float gap = 2.0f * below.fraction;
            if (gap < 0.49f) state.position.z += 0.49f - gap;
            else if (gap > 1.5f && gap < 2.0f) state.position.z -= gap - 1.5f;
        }
        state.velocity.z = c.jumpVelocity;
        state.grounded = false;
        state.justJumped = true;
    } else if (style.infiniteMidairJump && jumpPressed && !state.grounded && state.jumpCooldownTicks == 0) {
        // infinite.sp: overwrite (not add to) vertical velocity on a fresh jump press while airborne, exact value and cooldown from the live plugin
        state.velocity.z = 290.0f;
        state.jumpCooldownTicks = 3;
        state.justJumped = true;
    }
    if (state.jumpCooldownTicks > 0) {
        state.jumpCooldownTicks--;
    }

    if (state.grounded) {
        float speed = std::sqrt(state.velocity.x * state.velocity.x + state.velocity.y * state.velocity.y);
        if (speed > 0.1f) {
            float control = speed < c.stopSpeed ? c.stopSpeed : speed;
            float drop = control * c.friction * dt;
            float scale = std::max(speed - drop, 0.0f) / speed;
            state.velocity.x *= scale;
            state.velocity.y *= scale;
        } else {
            state.velocity.x = 0.0f;
            state.velocity.y = 0.0f;
        }
    }

    glm::vec3 forward = forwardFromYaw(input.yawDeg);
    glm::vec3 right = rightFromYaw(input.yawDeg);
    glm::vec3 wishDir(0.0f);
    if (input.forward) wishDir += forward;
    if (input.back) wishDir -= forward;
    if (input.right) wishDir += right;
    if (input.left) wishDir -= right;
    if (glm::length(wishDir) > 0.0001f) {
        wishDir = glm::normalize(wishDir);
    }

    float airAccelerate = style.airAccelerateOverride >= 0.0f ? style.airAccelerateOverride : c.airAccelerate;

    // the ground/air asymmetry here is THE bhop mechanic: grounded clamps to full maxSpeed, airborne clamps the wishspeed contribution to a much smaller cap (30 u/s, source's real engine constant, confirmed from test2dll.sp not guessed) so strafe jumping keeps adding speed past maxSpeed instead of being capped like ground movement
    float groundSpeed = state.ducking ? c.maxSpeed * c.duckSpeedMultiplier : c.maxSpeed;
    float wishSpeed = state.grounded ? groundSpeed : std::min(c.maxSpeed, c.airSpeedCap);
    float currentSpeed = glm::dot(state.velocity, wishDir);
    float addSpeed = wishSpeed - currentSpeed;
    state.speedGainedInAir = false;
    float horizontalSpeedBefore = glm::length(glm::vec2(state.velocity));
    if (addSpeed > 0.0f) {
        float accel = state.grounded ? c.accelerate : airAccelerate;
        float accelSpeed = std::min(accel * c.maxSpeed * dt, addSpeed);
        state.velocity += wishDir * accelSpeed;
        if (!state.grounded && glm::length(wishDir) > 0.0001f) {
            state.speedGainedInAir = true;
        }
    }

    // jhud gain: this tick's horizontal speed gain from air accelerate vs the best any wish direction could have done. adding speed a along a wishdir whose projection on velocity is d gives |v|^2 + 2ad + a^2, and the cap (addspeed >= a) allows at most d = wishSpeed - a
    state.airGain = 0.0f;
    state.airGainMax = 0.0f;
    if (!state.grounded) {
        float a = std::min(airAccelerate * c.maxSpeed * dt, wishSpeed);
        float d = wishSpeed - a;
        float v = horizontalSpeedBefore;
        state.airGainMax = std::sqrt(v * v + 2.0f * a * d + a * a) - v;
        state.airGain = glm::length(glm::vec2(state.velocity)) - v;
    }

    // test2dll.sp: while airborne (or grounded but holding jump, treated as an air tick) with attack + exactly one of a/d held and enough speed, work out the optimal strafe angle and snap the view to it. no need for the plugin's relative turn networking trick (that was to dodge prediction jitter), there's no network here so just output the absolute target yaw
    state.hasYawOverride = false;
    if (style.test2AutoStrafe) {
        bool airTick = !state.grounded || input.jump;
        bool exactlyOneStrafe = input.left != input.right;
        float speed = glm::length(glm::vec2(state.velocity.x, state.velocity.y));
        if (airTick && input.attack && exactlyOneStrafe && speed >= 10.0f) {
            float accelUncapped = airAccelerate * c.maxSpeed * dt;
            float wantedDot = c.airSpeedCap - accelUncapped;
            float theta = (wantedDot <= 0.0f)
                               ? glm::radians(90.0f)
                               : std::acos(std::clamp(wantedDot / speed, -1.0f, 1.0f));
            float thetaDeg = glm::degrees(theta);
            float velocityYawDeg = glm::degrees(std::atan2(state.velocity.y, state.velocity.x));
            float targetYaw = input.left ? (velocityYawDeg + thetaDeg - 90.0f) : (velocityYawDeg - thetaDeg + 90.0f);
            if (targetYaw > 180.0f) targetYaw -= 360.0f;
            else if (targetYaw < -180.0f) targetYaw += 360.0f;
            state.hasYawOverride = true;
            state.yawOverrideDeg = targetYaw;
        }
    }

    // source splits each tick's gravity around the move (StartGravity/FinishGravity), half before half after. that's what makes a jump peak at exactly v^2/2g (57 units) at any tickrate
    float gravity = c.gravity * style.gravityMultiplier;
    if (!state.grounded) {
        state.velocity.z -= gravity * dt * 0.5f;
    }

    // horizontal base velocity moves you without becoming your velocity (WalkMove/AirMove add it before TryPlayerMove and subtract it after)
    glm::vec3 baseHorizontal(state.baseVelocity.x, state.baseVelocity.y, 0.0f);
    glm::vec3 moveVelocity = state.velocity + baseHorizontal;
    state.preCollisionVelocity = state.velocity;
    bool wasGroundedBeforeMove = state.grounded;
    SlideResult moved = state.grounded ? walkMove(world, state.position, moveVelocity, halfExtents, dt)
                                       : slideMove(world, state.position, moveVelocity, halfExtents, dt, true);
    state.position = moved.pos;
    state.velocity = moved.vel - baseHorizontal;
    state.collidedThisTick = moved.blocked || (moved.hitFloor && !wasGroundedBeforeMove);

    // rngfix's edge fix: touching down on a platform's trailing edge and sliding off it in the same tick lost your vertical speed without ever letting you jump. if the rest of the tick ends with no ground under you, stay where you touched down instead, landed and able to jump (you can still walk off by not jumping)
    if (moved.landed && !moved.stoppedAtLanding) {
        collision::TraceResult below =
            collision::traceBox(world, state.position, state.position - glm::vec3(0.0f, 0.0f, 2.0f), halfExtents);
        bool groundBelow = !below.startSolid && below.fraction < 1.0f && below.normal.z > 0.7f;
        if (!groundBelow) {
            state.position = moved.landPos;
            state.velocity = moved.landVel - baseHorizontal;
        }
    }

    // landing: the slide already clipped the fall along the floor, which on a downhill slope is the speed gain slopes give
    if ((moved.hitFloor || moved.landed) && state.velocity.z <= 140.0f) {
        state.grounded = true;
    }

    // StayOnGround: while walking, snap down onto the surface below instead of drifting off it. without this a walkable slope can't be stood on (each tick moves you sideways off it, you fall back on, and the landing clip feeds you more downhill speed) and you'd float up to the 2 unit ground probe above flat floors
    if (state.grounded && !state.justJumped && state.velocity.z <= 0.0f) {
        glm::vec3 start = state.position + glm::vec3(0.0f, 0.0f, 2.0f);
        collision::TraceResult tr =
            collision::traceBox(world, start, state.position - glm::vec3(0.0f, 0.0f, kStepSize), halfExtents);
        if (!tr.startSolid && tr.fraction > 0.0f && tr.fraction < 1.0f && tr.normal.z >= 0.7f) {
            state.position.z = tr.endPos.z;
        }
    }

    if (!state.grounded) {
        state.velocity.z -= gravity * dt * 0.5f;
    }

    state.groundGap = 0.0f;
    if (state.grounded) {
        collision::TraceResult tr =
            collision::traceBox(world, state.position, state.position - glm::vec3(0.0f, 0.0f, 2.0f), halfExtents);
        if (!tr.startSolid && tr.fraction < 1.0f) state.groundGap = 2.0f * tr.fraction;
    }
}

}  // namespace movement
