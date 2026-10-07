#include "movement/triggers.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace movement {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool passesFilter(const collision::Trigger& t, const std::string& activatorName) {
    if (!t.filter) return true;
    return iequals(activatorName, t.filter->matchName) != t.filter->negated;
}

// "AddOutput <key> <value>" on !activator, the two forms bhop/surf maps use on players. basevelocity is the classic booster, targetname tags the player so later filter_activator_name checks route them by progress
void applyActivatorAddOutput(const std::string& param, TriggerRuntime& runtime, PlayerState& state) {
    size_t start = param.find_first_not_of(' ');
    if (start == std::string::npos) return;
    size_t keyEnd = param.find(' ', start);
    std::string key = param.substr(start, keyEnd == std::string::npos ? std::string::npos : keyEnd - start);
    std::string rest = keyEnd == std::string::npos ? "" : param.substr(keyEnd + 1);

    if (iequals(key, "basevelocity")) {
        std::istringstream iss(rest);
        glm::vec3 v(0.0f);
        if (iss >> v.x >> v.y >> v.z) {
            state.baseVelocity = v;
        }
    } else if (iequals(key, "targetname")) {
        size_t a = rest.find_first_not_of(' ');
        size_t b = rest.find_last_not_of(' ');
        runtime.activatorName = (a == std::string::npos) ? "" : rest.substr(a, b - a + 1);
    }
}

void fireOutputs(TriggerRuntime& runtime, const collision::Trigger& t, size_t triggerIndex, std::string_view event,
                 PlayerState& state) {
    for (size_t j = 0; j < t.outputs.size(); ++j) {
        const auto& out = t.outputs[j];
        if (out.event != event) continue;
        int& fired = runtime.outputFireCounts[triggerIndex][j];
        if (out.timesToFire > 0 && fired >= out.timesToFire) continue;
        fired++;
        if (iequals(out.target, "!activator") && iequals(out.input, "AddOutput")) {
            applyActivatorAddOutput(out.param, runtime, state);
        }
    }
}

bool overlapsTrigger(const collision::Trigger& t, const glm::vec3& center, const glm::vec3& halfExtents) {
    for (const auto& brush : t.brushes) {
        if (collision::aabbOverlapsBrush(brush, center, halfExtents)) return true;
    }
    return false;
}

constexpr int kTeleportPreserveAngles = 32;  // SF_TELEPORT_PRESERVE_ANGLES
constexpr int kPushOnce = 128;               // SF_TRIG_PUSH_ONCE

// mpbhops_but_working's timings: stand on a sinking block longer than this
// and its teleport takes you; after the cooldown the block counts as fresh.
constexpr float kBlockTeleportDelay = 0.06f;
constexpr float kBlockCooldown = 1.10f;

// CTriggerTeleport::Touch. it never zeroes velocity, valve's own code has "pVelocity = NULL; //BUGBUG - This does not set the player's velocity to zero!!!", which is why telehops keep their speed in css
void teleportPlayer(const collision::Trigger& t, bool teleportedLastTick, PlayerState& state,
                    const glm::vec3& halfExtents, TriggerTickResult& result, bool recorrectMomentum) {
    // rngfix's telehop fix: if you also hit the wall or floor behind a thin teleport this tick, the collision happened before the teleport was even checked, so keep the speed you'd have had without it. skipped right after another teleport (hubs that chain teleports to stop you)
    if (state.collidedThisTick && !teleportedLastTick) {
        state.velocity = state.preCollisionVelocity;
    }
    // destination origins are feet level, our position is the hull center
    state.position = t.destinationOrigin + glm::vec3(0.0f, 0.0f, halfExtents.z);
    if ((t.spawnflags & kTeleportPreserveAngles) == 0) {
        result.setView = true;
        result.pitchDeg = t.destinationAngles.x;
        result.yawDeg = t.destinationAngles.y;
        if (recorrectMomentum) {
            // keeping the velocity vector is what source does, and why a teleport facing a different way than you entered can spit you out moving backwards. turn the speed to face the destination instead, which is the way the view gets set to as well, so what you see and where you're going agree. vertical speed is left alone, that's gravity not aim
            float speed = glm::length(glm::vec2(state.velocity));
            float yaw = glm::radians(t.destinationAngles.y);
            state.velocity.x = std::cos(yaw) * speed;
            state.velocity.y = std::sin(yaw) * speed;
        }
    }
    result.teleported = true;
}

}  // namespace

void resetTriggerRuntime(TriggerRuntime& runtime, const collision::MapTriggers& triggers) {
    size_t n = triggers.triggers.size();
    runtime.touching.assign(n, false);
    runtime.disabled.assign(n, false);
    runtime.nextFireTime.assign(n, 0.0f);
    runtime.outputFireCounts.assign(n, {});
    for (size_t i = 0; i < n; ++i) {
        runtime.disabled[i] = triggers.triggers[i].startDisabled;
        runtime.outputFireCounts[i].assign(triggers.triggers[i].outputs.size(), 0);
    }
    runtime.activatorName.clear();
    runtime.time = 0.0f;
    runtime.teleportedLastTick = false;
    runtime.standingBlock = -1;
    runtime.lastBlock = -1;
    runtime.punishTime = 0.0f;
    runtime.health = 100.0f;
}

TriggerTickResult tickTriggers(TriggerRuntime& runtime, const collision::MapTriggers& triggers, PlayerState& state,
                               const MoveConstants& c, float dt, bool recorrectTeleportMomentum) {
    TriggerTickResult result;
    runtime.time += dt;
    glm::vec3 halfExtents(c.playerHalfWidth, c.playerHalfWidth, state.ducking ? c.duckHalfHeight : c.playerHalfHeight);
    bool teleportedLastTick = runtime.teleportedLastTick;
    runtime.teleportedLastTick = false;

    // rngfix's trigger jump fix: standing on the ground means the hull reaches it, even up to 2 units above, so triggers thinner than that gap (thin boosters, teleports) still fire
    glm::vec3 touchCenter = state.position;
    glm::vec3 touchHalf = halfExtents;
    if (state.grounded && state.groundGap > 0.0f) {
        touchCenter.z -= state.groundGap * 0.5f;
        touchHalf.z += state.groundGap * 0.5f;
    }

    // bhop blocks: a booster you just jumped off launches you, a sinking block you've stood on too long sends you through its teleport
    int previousBlock = runtime.standingBlock;
    runtime.standingBlock = -1;
    if (state.grounded) {
        glm::vec3 feetCenter = touchCenter - glm::vec3(0.0f, 0.0f, 0.5f);
        for (size_t b = 0; b < triggers.bhopBlocks.size() && runtime.standingBlock < 0; ++b) {
            for (const auto& brush : triggers.bhopBlocks[b].brushes) {
                if (collision::aabbOverlapsBrush(brush, feetCenter, touchHalf)) {
                    runtime.standingBlock = static_cast<int>(b);
                    break;
                }
            }
        }
    }
    if (state.justJumped && previousBlock >= 0 && triggers.bhopBlocks[previousBlock].boostSpeed > 0.0f) {
        state.baseVelocity.z += triggers.bhopBlocks[previousBlock].boostSpeed;
    }
    if (runtime.standingBlock >= 0) {
        const auto& block = triggers.bhopBlocks[runtime.standingBlock];
        if (block.teleportTrigger >= 0) {
            float sinceFirstTouch = runtime.time - runtime.punishTime;
            if (runtime.lastBlock != runtime.standingBlock || sinceFirstTouch > kBlockCooldown) {
                runtime.lastBlock = runtime.standingBlock;
                runtime.punishTime = runtime.time + kBlockTeleportDelay;
            } else if (sinceFirstTouch > kBlockTeleportDelay) {
                runtime.lastBlock = -1;
                const auto& tele = triggers.triggers[block.teleportTrigger];
                if (tele.hasDestination) {
                    runtime.teleportedLastTick = true;
                    teleportPlayer(tele, teleportedLastTick, state, halfExtents, result, recorrectTeleportMomentum);
                    return result;
                }
            }
        }
    }

    for (size_t i = 0; i < triggers.triggers.size(); ++i) {
        const auto& t = triggers.triggers[i];
        bool overlap = !runtime.disabled[i] && overlapsTrigger(t, touchCenter, touchHalf);

        if (!overlap) {
            if (runtime.touching[i]) {
                runtime.touching[i] = false;
                fireOutputs(runtime, t, i, "onendtouch", state);
            }
            continue;
        }

        // like CBaseTrigger: the filter gates entry (so the start/end touch outputs) and each trigger type re-checks it on every touch
        bool passes = passesFilter(t, runtime.activatorName);
        bool firstTouch = !runtime.touching[i];
        if (!runtime.touching[i] && passes) {
            runtime.touching[i] = true;
            fireOutputs(runtime, t, i, "onstarttouch", state);
            passes = passesFilter(t, runtime.activatorName);
        }
        if (!passes) continue;

        switch (t.kind) {
            case collision::TriggerKind::Teleport: {
                if (!t.hasDestination) break;
                runtime.teleportedLastTick = true;
                teleportPlayer(t, teleportedLastTick, state, halfExtents, result, recorrectTeleportMomentum);
                return result;
            }
            case collision::TriggerKind::Push: {
                glm::vec3 push = t.pushDir * t.pushSpeed;
                if (t.spawnflags & kPushOnce) {
                    state.velocity += push;
                    runtime.disabled[i] = true;
                    runtime.touching[i] = false;
                    break;
                }
                if (state.baseVelocityActive) {
                    push += state.baseVelocity;  // overlapping pushes this tick stack
                }
                if (push.z > 0.0f && state.grounded) {
                    state.grounded = false;
                    state.position.z += 1.0f;
                }
                state.baseVelocity = push;
                state.baseVelocityActive = true;
                break;
            }
            case collision::TriggerKind::Hurt: {
                // CTriggerHurt hurts on touch then every half second, a continuous drain per tick adds up to the same damage
                runtime.health -= firstTouch ? t.damage * 0.5f : t.damage * dt;
                runtime.health = std::min(runtime.health, 100.0f);  // negative damage heals, up to full
                if (runtime.health <= 0.0f) {
                    runtime.health = 100.0f;
                    result.killed = true;
                    return result;
                }
                break;
            }
            case collision::TriggerKind::Multiple:
            case collision::TriggerKind::Once: {
                if (runtime.time < runtime.nextFireTime[i]) break;
                fireOutputs(runtime, t, i, "ontrigger", state);
                if (t.kind == collision::TriggerKind::Once || t.wait <= 0.0f) {
                    runtime.disabled[i] = true;
                    runtime.touching[i] = false;
                } else {
                    runtime.nextFireTime[i] = runtime.time + t.wait;
                }
                break;
            }
        }
    }
    return result;
}

}  // namespace movement
