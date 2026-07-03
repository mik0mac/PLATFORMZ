#pragma once

#include "raylib.h"
#include "raymath.h"
#include "gamespace.h" // GameSpace, Player, Rocket
#include "input.h"     // PlayerInput, ApplyPlayerInput — bots feed the same path as humans
#include "constants.h"
#include "random.h"    // RandomFloat — per-frame aim jitter

//MARK: CONSTANTS
const float BOT_TICK_TIME = 1.0f;          // seconds between bot decision ticks.
const float BOT_ATTACK_DISTANCE = 80.0f;   // distance to which bots move when attacking.
const float BOT_ASTEROID_ATTACK_BUFFER = 20.0f; // don't fire at asteroids closer than this distance, to avoid self-damage.
const float BOT_ASTEROID_AVOID_BUFFER = 20.0f;    // distance to which bots will avoid asteroids.
const float BOT_ASTEROID_COLLISION_TIME_WINDOW = 3.0f; // worry about asteroids that will collide with bot within this window.
const float BOT_RETREAT_DISTANCE = 120.0f; // distance to which bots will retreat.
const float BOT_WALL_AVOID_BUFFER = 6.0f;  // steer along a wall once within this distance of it (> bot radius, so it triggers on approach, before the clamp pins the bot).
const float BOT_VERTICAL_THRESHOLD = 5.0f; // y-delta below which vertical corrections are ignored.
const float BOT_LOW_FUEL_THRESHOLD = 20.0f; // fuel level below which bots will seek a platform to land and regen.
const float BOT_LOW_HEALTH_THRESHOLD = 30.0f; // health level below which bots will retreat.
const float BOT_LOW_AMMO_THRESHOLD = 20.0f; // ammo level below which bots will avoid firing rockets.

// Sets out.jetpack / out.earthGravity based on vertical delta to a world-space
// target position. Called by any movement node that needs vertical intent —
// keeps the logic in one place since MoveToTarget and MoveToSafety both need it.
inline void applyVerticalIntent(float botY, float targetY, PlayerInput& out) {
    float dy = targetY - botY;
    if (dy > BOT_VERTICAL_THRESHOLD) {
        out.jetpack = true;      // target is above — thrust up
    } else if (dy < -BOT_VERTICAL_THRESHOLD) {
        out.earthGravity = true; // target is below — switch to faster gravity to descend
    }
    // within threshold: neither flag set, let gravity handle it naturally
}

// MARK: helpers
// Targets carry their collision radius under different member names (Player uses
// `radius`, Asteroid uses `size`); these overloads let the templated aim helpers
// read it uniformly without renaming either element.
inline float targetRadius(const Player& p)   { return p.radius; }
inline float targetRadius(const Asteroid& a) { return a.size; }

// Helper function to predict the future position of an object given its current position, velocity, and time.
Vector3 predictObjectFuturePosition (Vector3 position, Vector3 velocity, float time) {
    Vector3 displacement = Vector3Scale(velocity, time); // how far from starting position will it move in x time.
    return Vector3Add(displacement, position);
};

// Helper to calculate the entry and exit times for a single axis.
bool getAxisTimeWindow(float startPosA, float velA, float startPosB, float velB, float threshold, float& outMin, float& outMax) {
    float deltaP = startPosB - startPosA;
    float deltaV = velA - velB;

    if (std::abs(deltaV) < 1e-6f) {
        // Parallel movement on this axis: check if they are already within the threshold
        if (std::abs(deltaP) <= threshold) {
            outMin = 0.0f;
            outMax = std::numeric_limits<float>::infinity();
            return true;
        }
        return false; // Permanently missed on this axis
    }

    // Calculate the two times where the object crosses the threshold boundaries
    float t1 = (deltaP - threshold) / deltaV;
    float t2 = (deltaP + threshold) / deltaV;

    // Ensure outMin is the smaller time and outMax is the larger time
    outMin = std::min(t1, t2);
    outMax = std::max(t1, t2);

    return true;
}

// Returns the earliest time 't' where objects are within the {threshold.x, y, z} box
float calculateIntersectionWithAccuracy(Vector3 posA, Vector3 velA, Vector3 posB, Vector3 velB, Vector3 accuracy) {
    float xMin = 0.0f, xMax = 0.0f;
    float yMin = 0.0f, yMax = 0.0f;
    float zMin = 0.0f, zMax = 0.0f;

    // Get valid time windows for all three axes independently
    if (!getAxisTimeWindow(posA.x, velA.x, posB.x, velB.x, accuracy.x, xMin, xMax)) return -1.0f;
    if (!getAxisTimeWindow(posA.y, velA.y, posB.y, velB.y, accuracy.y, yMin, yMax)) return -1.0f;
    if (!getAxisTimeWindow(posA.z, velA.z, posB.z, velB.z, accuracy.z, zMin, zMax)) return -1.0f;

    // Find the intersection of all three time windows
    float overlapStart = std::max({xMin, yMin, zMin, 0.0f}); // Clamped to 0 for future-only predictions
    float overlapEnd = std::min({xMax, yMax, zMax});

    // If the combined window is valid, they overlap within the threshold
    if (overlapStart <= overlapEnd) {
        return overlapStart; // Earliest time they enter the accuracy zone
    }

    return -1.0f; // The windows do not overlap; they miss
}

template <typename TargetT>
Vector3 calculateLeadAim (const TargetT& target, Player& shooter, float projectileSpeed = ROCKET_SPEED, float time_window = BOT_TICK_TIME) {
    Vector3 shooterAimDirection = shooter.Forward(); // Get the shooter's forward direction 
    Vector3 projectileVelocity = Vector3Scale(shooterAimDirection, projectileSpeed);
    float r = targetRadius(target);
    float timeToImpact = calculateIntersectionWithAccuracy(shooter.position, projectileVelocity, target.position, target.velocity, {r, r, r});
    if (timeToImpact < 0.0f) {
        return shooterAimDirection; // No valid lead solution; aim directly at the target
    }
    Vector3 predictedTargetPosition = predictObjectFuturePosition(target.position, target.velocity, timeToImpact);
    Vector3 leadDirection = Vector3Subtract(predictedTargetPosition, shooter.position);
    return Vector3Normalize(leadDirection); // Return the normalized lead direction
}

// Per-axis approximate equality with an explicit threshold. Named distinctly
// from raymath's 2-arg Vector3Equals to avoid an ambiguous overload.
bool vec3ApproxEqual(Vector3 a, Vector3 b, float threshold = 1e-6f) {
    return (fabs(a.x - b.x) < threshold) && (fabs(a.y - b.y) < threshold) && (fabs(a.z - b.z) < threshold);
}

// Closest platform whose body covers the segment `from`->`to` (nullptr if the
// segment is clear). *outPerp (if given) receives the blocker's offset from the
// ray — callers that need to strafe around it use that; a cover check just needs
// the pointer. Shared by FindLineOfSight (clear a shot) and FindCover (hide).
inline const Platform* platformBlockingSegment(Vector3 from, Vector3 to,
        const std::vector<Platform>& platforms, float radiusMargin,
        Vector3* outPerp = nullptr) {
    Vector3 seg = Vector3Subtract(to, from);
    float len = Vector3Length(seg);
    if (len < 1e-4f) { if (outPerp) *outPerp = {0.0f, 0.0f, 0.0f}; return nullptr; }
    Vector3 dir = Vector3Scale(seg, 1.0f / len);
    const Platform* blocker = nullptr;
    float bestAlong = std::numeric_limits<float>::max();
    Vector3 bestPerp = {0.0f, 0.0f, 0.0f};
    for (const Platform& p : platforms) {
        Vector3 toP = Vector3Subtract(p.position, from);
        float along = Vector3DotProduct(toP, dir);           // projection onto the ray
        if (along <= 0.0f || along >= len) continue;         // behind `from` / past `to`
        if (len - along < EXPLOSION_DAMAGE_RADIUS) continue; // platform hugging `to`
        Vector3 perp = Vector3Subtract(toP, Vector3Scale(dir, along)); // offset from the ray
        // largest horizontal half-extent as a blocking radius, + caller's margin
        float blockRadius = 0.5f * fmaxf(p.size.x, p.size.z) + radiusMargin;
        if (Vector3Length(perp) > blockRadius) continue;     // ray misses this platform
        if (along < bestAlong) { blocker = &p; bestAlong = along; bestPerp = perp; }
    }
    if (outPerp) *outPerp = bestPerp;
    return blocker;
}

template <typename TargetT>
std::tuple<bool, Vector3> onTarget (const TargetT& target, Player& shooter, float threshold = 0.0f, float projectileSpeed = ROCKET_SPEED, float time_window = BOT_TICK_TIME) {
    Vector3 shooterAimDirection = shooter.Forward(); // Get the shooter's forward direction
    Vector3 newAimDirection = calculateLeadAim(target, shooter, projectileSpeed);
    if (vec3ApproxEqual(shooterAimDirection, newAimDirection)) {
        Vector3 targetFuturePosition = predictObjectFuturePosition(target.position, target.velocity, time_window);
        newAimDirection = Vector3Subtract(targetFuturePosition, shooter.position);
        newAimDirection = Vector3Normalize(newAimDirection);
        return {false, newAimDirection}; // Not on target, but provide the new aim direction to lead the target.
    }
    return {true, newAimDirection}; // On target at new aim.
}


//
enum class Status { Success, Failure, Running };

// Generic per-bot node state. Lives outside the (shared) tree nodes so each bot
// keeps its own latch/timer. Each bot holds a vector of these — one slot per
// stateful node in the tree, indexed by that node's latchId/stateId — so
// nested/sibling nodes don't clobber each other's state. Shared by
// LatchedSelector, WeightedRandomSelector (branch latch), Cooldown (timer), and
// Chance (open/closed latch); each node reads the fields to suit.
struct BotDecision {
    float timer = 0.0f;         // seconds accumulated since the last decision (or since a Cooldown's last success)
    float interval = BOT_TICK_TIME; // this decision's jittered period (set on each re-decide)
    int   activeBranch = -1;    // latched child index / Chance open(1)/closed(0) / Cooldown init flag (-1 = decide now)
};

// Per-bot personality. Assigned once at spawn (seeded from player.id, see
// main.cpp startGame) and read by the nodes to scale the global BOT_* baselines.
// Like BotDecision, it lives per-bot and rides in the Blackboard rather than in
// the shared tree nodes.
struct BotProfile {
    float aggression = 0.6f; // 0 timid/kites .. 1 reckless/in-your-face
    float accuracy   = 0.6f; // 0 sprays wildly .. 1 snaps on target
};

//MARK: blackboard
// Per-bot, per-tick context. Nodes never touch bot.position/bot.velocity
// directly — they write intent into `out`, which flows through the same
// ApplyPlayerInput() path a human's PollLocalInput() does.
template <typename TargetT>
struct Blackboard {
    Player& bot;
    const TargetT& target;
    const std::vector<Player>& allPlayers; // for MoveToSafety's avoidance scan
    const std::vector<Platform>& allPlatforms;
    const std::vector<Asteroid>& allAsteroids;
    const Walls& walls;
    PlayerInput& out;
    float dt;
    std::vector<BotDecision>& decisions; // per-bot latch/timer, one slot per LatchedSelector (indexed by its latchId)
    const BotProfile& profile;  // per-bot personality (aggression/accuracy)
};

template <typename TargetT>
class Node {
public:
    virtual ~Node() = default;
    virtual Status tick(Blackboard<TargetT>& bb) = 0;
};
// MARK: Find Line of Sight
template <typename TargetT>
class FindLineOfSight : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        // Is there already a clear shot? If no platform covers the bot->target
        // segment, hold position and let fireAtTarget (top Parallel) take the shot.
        // blockerPerp is the blocker's offset from the ray — used to strafe around it.
        Vector3 blockerPerp;
        const Platform* blocker = platformBlockingSegment(bb.bot.position, bb.target.position,
                                                          bb.allPlatforms, bb.bot.radius, &blockerPerp);
        if (!blocker) return Status::Success; // clear line of sight — hold and snipe

        Vector3 los = Vector3Normalize(Vector3Subtract(bb.target.position, bb.bot.position));

        // Strafe opposite the platform's offset so it slides off the ray. The
        // direction is derived purely from geometry, so the same choice recurs each
        // tick and the bot converges — no random vantage point, no wandering. A
        // dead-center blocker (zero offset) gets a fixed tie-break, not a random pick.
        Vector3 strafe;
        if (Vector3Length(blockerPerp) > 1e-3f) {
            strafe = Vector3Normalize(Vector3Negate(blockerPerp));
        } else {
            strafe = Vector3CrossProduct(los, {0.0f, 1.0f, 0.0f}); // horizontal perpendicular to LOS
            if (Vector3Length(strafe) < 1e-3f) strafe = {1.0f, 0.0f, 0.0f};
            strafe = Vector3Normalize(strafe);
        }

        Vector3 fwd = bb.bot.ForwardFlat();
        Vector3 right = bb.bot.Right();
        bb.out.moveAxis.y = Vector3DotProduct(strafe, fwd);
        bb.out.moveAxis.x = Vector3DotProduct(strafe, right);
        // strafe.y != 0 when going over/under is the shorter way off the ray;
        // applyVerticalIntent realizes that via jetpack/earthGravity.
        applyVerticalIntent(bb.bot.position.y, bb.bot.position.y + strafe.y, bb.out);
        return Status::Running;
    }
};

// MARK: Find cover
// Essentially the inverse of FindLineOfSight.
template <typename TargetT>
class FindCover : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        if (bb.allPlatforms.empty()) return Status::Failure; // no cover to seek — let the Selector fall through

        // Already behind cover? (a platform covers the bot<->target segment) — hold
        // position; the top Parallel keeps firing back from cover.
        if (platformBlockingSegment(bb.bot.position, bb.target.position, bb.allPlatforms, bb.bot.radius))
            return Status::Success;

        // Otherwise pick the closest platform and move to its far side from the target.
        const Platform* closest = nullptr;
        float best = std::numeric_limits<float>::max();
        for (const Platform& platform : bb.allPlatforms) {
            float d = Vector3Length(Vector3Subtract(platform.position, bb.bot.position));
            if (d < best) { closest = &platform; best = d; }
        }

        // Direction target->platform, continued past the platform to its far face; the
        // EXPLOSION_DAMAGE_RADIUS standoff keeps the bot clear of splash on the near face.
        Vector3 targetToPlat = Vector3Subtract(closest->position, bb.target.position);
        if (Vector3Length(targetToPlat) < 1e-3f) return Status::Failure; // platform ~on the target: useless as cover
        targetToPlat = Vector3Normalize(targetToPlat);
        Vector3 coveredPos = Vector3Add(closest->position, Vector3Scale(targetToPlat, EXPLOSION_DAMAGE_RADIUS));

        Vector3 dir = Vector3Normalize(Vector3Subtract(coveredPos, bb.bot.position));
        Vector3 fwd = bb.bot.ForwardFlat();
        Vector3 right = bb.bot.Right();
        bb.out.moveAxis.y = Vector3DotProduct(dir, fwd);
        bb.out.moveAxis.x = Vector3DotProduct(dir, right);
        applyVerticalIntent(bb.bot.position.y, coveredPos.y, bb.out);
        return Status::Running;
    }
};
        



//MARK: Move To Player
// Writes out.moveAxis (local strafe/forward, not world space) — see
// Player::updateVelocity for the axes this is projected onto.
template <typename TargetT>
class MoveToPlayer : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        Vector3 toTarget = Vector3Subtract(bb.target.position, bb.bot.position);
        float dist = Vector3Length(toTarget);
        // Aggressive bots close right in (smaller stop distance); timid bots
        // hold their distance. aggression 0 -> 1.5x baseline, 1 -> 0.5x.
        float stopDist = BOT_ATTACK_DISTANCE * (1.5f - bb.profile.aggression);
        if (dist < stopDist) return Status::Success; // close enough, stop closing in
        // Determine the Bot's next move.
        Vector3 dir = Vector3Normalize(toTarget);
        Vector3 fwd = bb.bot.ForwardFlat();
        Vector3 right = bb.bot.Right();
        bb.out.moveAxis.y = Vector3DotProduct(dir, fwd);
        bb.out.moveAxis.x = Vector3DotProduct(dir, right);
        applyVerticalIntent(bb.bot.position.y, bb.target.position.y, bb.out);
        return Status::Running;
    }
};

//MARK: Move From Player
// Steers away from any player within BOT_RETREAT_DISTANCE, weighted by
// proximity. Writes out.moveAxis like MoveToTarget — same local-space rule.
template <typename TargetT>
class MoveFromPlayer : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        Vector3 totalAvoidance = {0, 0, 0};
        for (const Player& opponent : bb.allPlayers) {
            if (opponent.id == bb.bot.id) continue; // don't avoid yourself
            Vector3 away = Vector3Subtract(bb.bot.position, opponent.position);
            float dist = Vector3Length(away);
            if (dist < BOT_RETREAT_DISTANCE && dist > 0.0f) {
                float weight = (BOT_RETREAT_DISTANCE - dist) / BOT_RETREAT_DISTANCE;
                totalAvoidance = Vector3Add(totalAvoidance, Vector3Scale(Vector3Normalize(away), weight));
            }
        }
        if (Vector3LengthSqr(totalAvoidance) <= 0.0f) return Status::Failure; // no threat — not applicable, let the Selector fall through to Attack etc..
        // Don't steer into a boundary: if the escape direction points at a wall the
        // bot is already near, drop that component and slide along the wall instead.
        // The cube is symmetric (-halfSize..+halfSize on every axis). Trigger on
        // approach (BOT_WALL_AVOID_BUFFER), not just at contact — the collision clamp
        // otherwise pins the bot right at the wall before this could ever fire.
        float bound = bb.walls.halfSize - BOT_WALL_AVOID_BUFFER;

        Vector3 dir = Vector3Normalize(totalAvoidance);
        bool blockedX = false;
        bool blockedY = false;
        bool blockedZ = false;
        if (dir.x < 0.0f && bb.bot.position.x < -bound) blockedX = true;
        if (dir.x > 0.0f && bb.bot.position.x >  bound) blockedX = true;
        if (dir.y < 0.0f && bb.bot.position.y < -bound) blockedY = true;
        if (dir.y > 0.0f && bb.bot.position.y >  bound) blockedY = true;
        if (dir.z < 0.0f && bb.bot.position.z < -bound) blockedZ = true;
        if (dir.z > 0.0f && bb.bot.position.z >  bound) blockedZ = true;
        // Cornered on every axis: give up here so the Selector tries another branch.
        if (blockedX && blockedY && blockedZ) return Status::Failure;
        // Zero the into-wall components and renormalize so the bot slides along the wall.
        if (blockedX) dir.x = 0.0f;
        if (blockedY) dir.y = 0.0f;
        if (blockedZ) dir.z = 0.0f;
        dir = Vector3Normalize(dir);

        Vector3 fwd = bb.bot.ForwardFlat();
        Vector3 right = bb.bot.Right();
        bb.out.moveAxis.y = Vector3DotProduct(dir, fwd);
        bb.out.moveAxis.x = Vector3DotProduct(dir, right);
        // Vertical intent uses the same escape magnitude, but zeroed when the ceiling
        // /floor is the blocked side, so the bot won't thrust into a boundary.
        float verticalAvoid = blockedY ? 0.0f : totalAvoidance.y;
        applyVerticalIntent(bb.bot.position.y, bb.bot.position.y + verticalAvoid, bb.out);
        return Status::Running;
    }
};

//MARK: Move To Platform
template <typename TargetT>
class MoveToPlatform : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        if (bb.allPlatforms.empty()) return Status::Failure;

        // find the closest platform
        const Platform* closestPlatform = nullptr;
        float closestDist = std::numeric_limits<float>::max();
        for (const Platform& platform : bb.allPlatforms) {
            float dist = Vector3Length(Vector3Subtract(platform.position, bb.bot.position));
            if (dist < closestDist) {
                closestPlatform = &platform;
                closestDist = dist;
            }
        }

        // target a landing point one bot-radius above the platform surface
        Vector3 landingPosition = closestPlatform->position;
        landingPosition.y += (bb.bot.radius * 2.0f);
        // becuase of the threhold of equality, all points will be above the platform surface.
        // On success, the bot will move to Idle and fall to the platform surface.
        // might need to add something to counteract the bot's momentum if it is also moving horizontally.

        // considered successful if the bot is at the target point (within its radius).  Third arg is the threshold for equality.
        if (vec3ApproxEqual(bb.bot.position, landingPosition, bb.bot.radius)) return Status::Success; // on platform, done

        Vector3 dir = Vector3Normalize(Vector3Subtract(landingPosition, bb.bot.position));
        Vector3 fwd = bb.bot.ForwardFlat();
        Vector3 right = bb.bot.Right();
        bb.out.moveAxis.y = Vector3DotProduct(dir, fwd);
        bb.out.moveAxis.x = Vector3DotProduct(dir, right);
        applyVerticalIntent(bb.bot.position.y, landingPosition.y, bb.out);
        return Status::Running;
    }
};

//MARK: Idle
template <typename TargetT>
class Idle : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        bb.out.moveAxis = {0, 0};
        bb.out.jetpack = false;
        bb.out.earthGravity = false;
        return Status::Success; // idle is a valid resting state, not an ongoing action
    }
};

// Steers the bot's yaw AND pitch one clamped step each toward aimDir (a 3D
// direction), writing out.lookDelta. Adds a personality-scaled random spread so
// low-accuracy bots don't track perfectly (accuracy 1 -> no spread, accuracy 0
// -> +/-BOT_MAX_AIM_SPREAD, applied to both axes). Shared by FireAtPlayer and
// AttackAsteroid.
template <typename TargetT>
inline void steerAimToward(Blackboard<TargetT>& bb, Vector3 aimDir) {
    float maxStep = BOT_TURN_RATE * bb.dt;

    // yaw: horizontal heading from the x/z components
    float desiredYaw = atan2f(aimDir.z, aimDir.x);
    desiredYaw += RandomFloat(-1.0f, 1.0f) * BOT_MAX_AIM_SPREAD * (1.0f - bb.profile.accuracy);
    float yawError = desiredYaw - bb.bot.yaw;
    while (yawError > PI) yawError -= 2.0f * PI;
    while (yawError < -PI) yawError += 2.0f * PI;
    float yawStep = Clamp(yawError, -maxStep, maxStep);
    bb.out.lookDelta.x = yawStep / bb.bot.lookSensitivity;

    // pitch: aimDir is normalized, so aimDir.y == sin(desiredPitch). Pitch is
    // bounded (not cyclic), so no PI-wrap; clamp to the same limit updateLook enforces.
    float desiredPitch = asinf(Clamp(aimDir.y, -1.0f, 1.0f));
    desiredPitch += RandomFloat(-1.0f, 1.0f) * BOT_MAX_AIM_SPREAD * (1.0f - bb.profile.accuracy);
    desiredPitch = Clamp(desiredPitch, -bb.bot.pitchLimit, bb.bot.pitchLimit);
    float pitchError = desiredPitch - bb.bot.pitch;
    float pitchStep  = Clamp(pitchError, -maxStep, maxStep);
    // updateLook() does `pitch -= lookDelta.y * lookSensitivity`, so negate to
    // raise aim toward desiredPitch.
    bb.out.lookDelta.y = -pitchStep / bb.bot.lookSensitivity;
}

//MARK: Fire At Player
// Writes out.lookDelta (steers yaw toward the lead-aim solution) and
// out.fire once aligned.
template <typename TargetT>
class FireAtPlayer : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        // No shot through a platform: don't aim or fire, and fail so the
        // fireAtTarget selector can fall through to shooting asteroids instead.
        if (platformBlockingSegment(bb.bot.position, bb.target.position, bb.allPlatforms, bb.bot.radius))
            return Status::Failure;
        // no ammo, can't fire
        if (bb.bot.ammo <= 0) return Status::Failure; 
        // A bot with low health returns failure to the fireAtTarget selector, which will fall through to
        // firing asteroids instead of the player.  The threshold is scaled by the bot's aggression,
        // so a more aggressive bot will continue to fire at the player longer with lower health than a timid bot.
        if (bb.bot.health < BOT_LOW_HEALTH_THRESHOLD * (1.0f - 0.8f * bb.profile.aggression)) return Status::Failure;

        auto [isOnTarget, aimDir] = onTarget(bb.target, bb.bot);
        steerAimToward(bb, aimDir);

        if (isOnTarget) {
            if (!DISABLE_BOT_FIRE_PLAYER) bb.out.fire = true;
            return Status::Success;
        }
        return Status::Running;
    }
};

//MARK: attack asteroid
template <typename TargetT>
class AttackAsteroid : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        if (bb.allAsteroids.empty()) return Status::Failure; // no asteroids to attack
        if (bb.bot.ammo <= 0) return Status::Failure; // no ammo, can't fire

        // find the closest asteroid that is not within the bot's buffer.
        // Aggressive bots accept a smaller safety buffer (riskier close shots).
        float buffer = BOT_ASTEROID_ATTACK_BUFFER * (1.0f - 0.5f * bb.profile.aggression);
        const Asteroid* closestAsteroid = nullptr;
        float closestDist = std::numeric_limits<float>::max();
        for (const Asteroid& asteroid : bb.allAsteroids) {
            float dist = Vector3Length(Vector3Subtract(asteroid.position, bb.bot.position));
            if (dist < closestDist && dist > buffer) {
                // no shot through a platform — skip and keep looking for a clear one
                if (platformBlockingSegment(bb.bot.position, asteroid.position, bb.allPlatforms, bb.bot.radius)) continue;
                closestAsteroid = &asteroid;
                closestDist = dist;
            }
        }

        //find aiming scheme to lead the asteroid
        if (closestAsteroid) {
            auto [isOnTarget, aimDir] = onTarget(*closestAsteroid, bb.bot);
            steerAimToward(bb, aimDir);
            if (isOnTarget) {
                if (!DISABLE_BOT_FIRE_ASTEROIDS) bb.out.fire = true;
                return Status::Success; // firing!
            }
            return Status::Running; // aiming solution exists, but not yet aligned.
        }
        return Status::Failure;  // no valid asteroid to attack
    }
};

//MARK: avoid asteroid
template <typename TargetT>
class AvoidAsteroid : public Node<TargetT> {
    public:
    Status tick(Blackboard<TargetT>& bb) override {
        if (bb.allAsteroids.empty()) return Status::Failure;

        float buffer = BOT_ASTEROID_AVOID_BUFFER * (1.0f - 0.5f * bb.profile.aggression);
        std::vector<Asteroid> dangerousAsteroids;
        
        for (const Asteroid& asteroid : bb.allAsteroids) {
            // find all close asteroids (within buffer)
            float dist = Vector3Length(Vector3Subtract(asteroid.position, bb.bot.position));
            float r = asteroid.size + bb.bot.radius;
            if (dist < buffer) {
                float time = calculateIntersectionWithAccuracy(asteroid.position, asteroid.velocity, bb.bot.position, bb.bot.velocity, {r, r, r});
                // Keep only imminent collisions: time < 0 is a miss, time > window is
                // too far out to worry about. time == 0 means already inside the zone.
                if (time < 0.0f || time > BOT_ASTEROID_COLLISION_TIME_WINDOW) continue;

                // asteroid is close and will collide within the time window.  Add to list:
                dangerousAsteroids.push_back(asteroid);
            }
        }
        // no danger, move on.
        if (dangerousAsteroids.empty()) return Status::Failure;

        // Steer away from where the asteroids actually are, not where they're
        // heading. Sum the bot->asteroid vectors weighted by proximity (closer =
        // more urgent), then flee the opposite way.
        Vector3 threatDir = {0.0f, 0.0f, 0.0f};
        for (const Asteroid& asteroid : dangerousAsteroids) {
            Vector3 toAsteroid = Vector3Subtract(asteroid.position, bb.bot.position);
            float dist = Vector3Length(toAsteroid);
            if (dist < 1e-4f) continue; // overlapping; no meaningful direction
            // weight = 1/dist so nearer asteroids dominate the escape vector
            threatDir = Vector3Add(threatDir, Vector3Scale(toAsteroid, 1.0f / (dist * dist)));
        }
        // If the threats cancel out (e.g. converging from opposite sides), pick
        // any escape rather than steering toward a zero/garbage direction.
        if (Vector3Length(threatDir) < 1e-4f) {
            threatDir = Vector3Negate(bb.bot.velocity);
            if (Vector3Length(threatDir) < 1e-4f) threatDir = {0.0f, 1.0f, 0.0f};
        }
        Vector3 awayDir = Vector3Normalize(Vector3Negate(threatDir));
        steerAimToward(bb, awayDir);
        return Status::Running;
    }
};

//MARK: is low fuel
template <typename TargetT>
class IsLowFuel : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        return bb.bot.fuel < BOT_LOW_FUEL_THRESHOLD ? Status::Success : Status::Failure;
    }
};

//MARK: is low health
template <typename TargetT>
class IsLowHealth : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        // Aggressive bots tolerate more damage before retreating. Keep a small
        // floor so even the most reckless bot still bails when nearly dead.
        float thresh = BOT_LOW_HEALTH_THRESHOLD * (1.0f - 0.8f * bb.profile.aggression);
        return bb.bot.health < thresh ? Status::Success : Status::Failure;
    }
};

//MARK: Needs Bonus
template <typename TargetT>
class NeedsBonus : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        // A bot benefits from an asteroid bonus when a resource is below max by more
        // than the bonus would grant, scaled by aggression: a reckless bot grabs
        // bonuses even when barely down; a timid bot waits until it can bank the full
        // value. Guard node -> Success when a bonus is wanted, Failure when topped up.
        bool needsBonus =
            bb.bot.health < PLAYER_MAX_HEALTH - (ASTEROID_HEALTH_AWARD * (1.0f - bb.profile.aggression)) ||
            bb.bot.fuel   < PLAYER_MAX_FUEL   - (ASTEROID_FUEL_AWARD   * (1.0f - bb.profile.aggression)) ||
            bb.bot.ammo   < PLAYER_MAX_AMMO   - (ASTEROID_AMMO_AWARD   * (1.0f - bb.profile.aggression));
        return needsBonus ? Status::Success : Status::Failure;
    }
};

//MARK: Composites
// Mutually exclusive children — only one ever actually writes movement
// per tick (the first non-failure wins, rest aren't reached). Use for
// movement priority: Evade > Attack-move > Patrol.
template <typename TargetT>
class Selector : public Node<TargetT> {
    std::vector<Node<TargetT>*> children;
public:
    Selector(std::vector<Node<TargetT>*> kids) : children(std::move(kids)) {}
    Status tick(Blackboard<TargetT>& bb) override {
        for (auto* c : children) {
            Status s = c->tick(bb);
            if (s != Status::Failure) return s; // first applicable child wins
        }
        return Status::Failure;
    }
};

// Like Selector, but throttles only the DECISION — which child wins — to a
// BOT_TICK_TIME cadence. Between decisions it re-ticks the latched child every
// frame, so the chosen action's movement/aim/fire stay per-dt; only the branch
// choice is frozen (~1s). The latch lives per-bot in bb.decisions[latchId] (not
// in this shared node), so one tree instance drives every bot independently and
// multiple/nested LatchedSelectors each keep their own latch slot.
template <typename TargetT>
class LatchedSelector : public Node<TargetT> {
    int latchId;                           // slot into bb.decisions — unique per LatchedSelector
    std::vector<Node<TargetT>*> children;
public:
    LatchedSelector(int id, std::vector<Node<TargetT>*> kids) : latchId(id), children(std::move(kids)) {}
    Status tick(Blackboard<TargetT>& bb) override {
        BotDecision& d = bb.decisions[latchId];
        d.timer += bb.dt;

        // (Re)decide on first tick or once this decision's interval elapses: pick
        // the first child that doesn't fail. Ticking to decide also executes that
        // child this frame, which is fine. The interval is jittered per decision
        // so bots don't all re-think in lockstep.
        if (d.activeBranch < 0 || d.timer >= d.interval) {
            d.timer = (d.activeBranch < 0) ? 0.0f : d.timer - d.interval; // carry, don't zero
            d.interval = BOT_TICK_TIME * RandomFloat(BOT_TICK_JITTER_MIN, BOT_TICK_JITTER_MAX);
            d.activeBranch = -1;
            for (int i = 0; i < (int)children.size(); ++i) {
                if (children[i]->tick(bb) != Status::Failure) { d.activeBranch = i; break; }
            }
            return d.activeBranch >= 0 ? Status::Running : Status::Failure;
        }

        // Between decisions: run the latched child. If a guard flipped and it now
        // fails, re-decide immediately rather than coast (leaving the bot inert).
        if (d.activeBranch >= 0 && d.activeBranch < (int)children.size()) {
            Status s = children[d.activeBranch]->tick(bb);
            if (s != Status::Failure) return s;
            d.timer = d.interval; // force a fresh decision next tick
        }
        return Status::Failure;
    }
};

// Like LatchedSelector, but the branch choice each decision is WEIGHTED RANDOM
// instead of first-non-failing. Children are evaluated in a random order biased
// by their weight; the first that doesn't fail is latched for the (jittered)
// decision window — so it still honors the Failure = not-applicable convention,
// but two bots with the same tree diverge. Weight is a captureless function
// pointer so it can read bb.profile (personality) with no <functional> cost.
// Latch/timer live per-bot in bb.decisions[latchId], same as LatchedSelector.
template <typename TargetT>
class WeightedRandomSelector : public Node<TargetT> {
    int latchId;                            // slot into bb.decisions — unique per node
    struct Child { Node<TargetT>* node; float (*weight)(Blackboard<TargetT>&); };
    std::vector<Child> children;
public:
    WeightedRandomSelector(int id, std::vector<Child> kids) : latchId(id), children(std::move(kids)) {}
    Status tick(Blackboard<TargetT>& bb) override {
        BotDecision& d = bb.decisions[latchId];
        d.timer += bb.dt;

        if (d.activeBranch < 0 || d.timer >= d.interval) {         // (re)decide
            d.timer = (d.activeBranch < 0) ? 0.0f : d.timer - d.interval; // carry, don't zero
            d.interval = BOT_TICK_TIME * RandomFloat(BOT_TICK_JITTER_MIN, BOT_TICK_JITTER_MAX);
            d.activeBranch = -1;
            // Weighted pick-without-replacement; first non-failing pick wins & latches.
            std::vector<int> pool;
            for (int i = 0; i < (int)children.size(); ++i) pool.push_back(i);
            while (!pool.empty()) {
                float total = 0.0f;
                for (int i : pool) total += fmaxf(0.0f, children[i].weight(bb));
                float r = RandomFloat(0.0f, total), acc = 0.0f;
                size_t k = pool.size() - 1;                        // fallback if total==0
                for (size_t j = 0; j < pool.size(); ++j) {
                    acc += fmaxf(0.0f, children[pool[j]].weight(bb));
                    if (r <= acc) { k = j; break; }
                }
                int pick = pool[k];
                pool.erase(pool.begin() + k);
                if (children[pick].node->tick(bb) != Status::Failure) { d.activeBranch = pick; break; }
            }
            return d.activeBranch >= 0 ? Status::Running : Status::Failure;
        }

        // Between decisions: run the latched child; if its guard flipped and it
        // now fails, force a fresh decision next frame rather than coast.
        if (d.activeBranch >= 0 && d.activeBranch < (int)children.size()) {
            Status s = children[d.activeBranch].node->tick(bb);
            if (s != Status::Failure) return s;
            d.timer = d.interval;
        }
        return Status::Failure;
    }
};

//MARK: Decorators
// Single-child wrappers that transform their child's result. Like the
// composites, they're shared across bots and keep per-bot state in a
// bb.decisions[stateId] slot (see BotDecision).

// Rate-gates its child: returns Failure until `cooldown` seconds have passed
// since the child last returned Success, so a parent Selector falls through in
// the meantime. Available immediately on spawn, then blocked after each success.
template <typename TargetT>
class Cooldown : public Node<TargetT> {
    int stateId; float cooldown; Node<TargetT>* child;
    float (*periodFn)(Blackboard<TargetT>&) = nullptr; // per-bot period (s); overrides `cooldown` when set
public:
    Cooldown(int id, float seconds, Node<TargetT>* c) : stateId(id), cooldown(seconds), child(c) {}
    // Per-bot variant: the period is computed from the blackboard each tick (e.g.
    // scaled by bb.profile), since the shared tree node can't bake a per-bot value.
    Cooldown(int id, float (*period)(Blackboard<TargetT>&), Node<TargetT>* c)
        : stateId(id), cooldown(0.0f), child(c), periodFn(period) {}
    Status tick(Blackboard<TargetT>& bb) override {
        BotDecision& s = bb.decisions[stateId];
        float cd = periodFn ? periodFn(bb) : cooldown;             // per-bot period when a fn is supplied
        if (s.activeBranch < 0) { s.activeBranch = 0; s.timer = cd; } // fresh: ready now
        s.timer += bb.dt;
        if (s.timer < cd) return Status::Failure;                  // still cooling down
        Status st = child->tick(bb);
        if (st == Status::Success) s.timer = 0.0f;                 // restart cooldown on completion
        return st;
    }
};

// Probabilistic gate: rolls once per (jittered) decision window — with
// probability p the child is "open" (passes through) for that window, else
// Failure. Latching the roll (not re-rolling per frame) keeps the gated action
// from flickering on and off.
template <typename TargetT>
class Chance : public Node<TargetT> {
    int stateId; float probability; Node<TargetT>* child;
public:
    Chance(int id, float p, Node<TargetT>* c) : stateId(id), probability(p), child(c) {}
    Status tick(Blackboard<TargetT>& bb) override {
        BotDecision& s = bb.decisions[stateId];
        s.timer += bb.dt;
        if (s.activeBranch < 0 || s.timer >= s.interval) {         // (re)roll open/closed
            s.timer = (s.activeBranch < 0) ? 0.0f : s.timer - s.interval;
            s.interval = BOT_TICK_TIME * RandomFloat(BOT_TICK_JITTER_MIN, BOT_TICK_JITTER_MAX);
            s.activeBranch = (RandomFloat(0.0f, 1.0f) < probability) ? 1 : 0; // 1=open, 0=closed
        }
        return s.activeBranch == 1 ? child->tick(bb) : Status::Failure;
    }
};

template <typename TargetT>
class Sequence : public Node<TargetT> {
    std::vector<Node<TargetT>*> children;
public:
    Sequence(std::vector<Node<TargetT>*> kids) : children(std::move(kids)) {}
    Status tick(Blackboard<TargetT>& bb) override {
        for (auto* c : children) {
            Status s = c->tick(bb);
            if (s != Status::Success) return s; // Failure or Running short-circuits
        }
        return Status::Success;
    }
};

// Ticks EVERY child every frame regardless of the others' results — for
// concurrent behaviors that aren't mutually exclusive (e.g. movement and
// firing both run the same tick, since a bot can attack-move AND fire,
// or retreat AND fire).
template <typename TargetT>
class Parallel : public Node<TargetT> {
    std::vector<Node<TargetT>*> children;
public:
    Parallel(std::vector<Node<TargetT>*> kids) : children(std::move(kids)) {}
    Status tick(Blackboard<TargetT>& bb) override {
        bool anyRunning = false;
        for (auto* c : children) {
            if (c->tick(bb) == Status::Running) anyRunning = true;
        }
        return anyRunning ? Status::Running : Status::Success;
    }
};


//MARK: Entry point
// Ticks the tree once and returns a PlayerInput ready for ApplyPlayerInput() —
// same destination as a human's PollLocalInput() or the wander BotState path.
template <typename TargetT>
PlayerInput botInput(Player& bot,
                    const TargetT& target,
                    const std::vector<Player>& allPlayers,
                    const std::vector<Platform>& platforms,
                    const std::vector<Asteroid>& asteroids,
                    const Walls& walls,
                    Node<TargetT>& tree,
                    float dt,
                    std::vector<BotDecision>& decisions,
                    const BotProfile& profile) {
    PlayerInput out;
    Blackboard<TargetT> bb{bot, target, allPlayers, platforms, asteroids, walls, out, dt, decisions, profile};
    tree.tick(bb);
    return out;
}

