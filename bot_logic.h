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
const float BOT_BEARING_DOWN_RANGE = 50.0f; // an opponent within this range AND closing counts as "bearing down" — gates the kite (MoveFromPlayer).
const float BOT_AGGRO_SKIP_DEFENSE = 0.85f; // how strongly aggression suppresses the low-health retreat: p(defend) = 1 - this*aggression (aggro 1 -> 15% defend, aggro 0 -> always defend).
const float BOT_WALL_AVOID_BUFFER = 6.0f;  // steer along a wall once within this distance of it (> bot radius, so it triggers on approach, before the clamp pins the bot).
const float BOT_WALL_CLEAR_MARGIN = 6.0f;  // extra depth past the avoid buffer a bot must reach before it stops peeling off a wall (hysteresis, so it commits instead of jittering at the edge).
const float BOT_WALL_WANDER_STRENGTH = 0.6f; // lateral blend into the off-wall departure direction (0 = straight inward, higher = more sideways drift, for varied exit paths).
const float BOT_VERTICAL_THRESHOLD = 5.0f; // y-delta below which vertical corrections are ignored.
const float BOT_LOW_FUEL_THRESHOLD = 20.0f; // fuel level below which bots will seek a platform to land and regen.
const float BOT_LOW_HEALTH_THRESHOLD = 30.0f; // health level below which bots will retreat.
const float BOT_LOW_AMMO_THRESHOLD = 20.0f; // ammo level below which bots will avoid firing rockets.

const float BOT_FUEL_SCARCITY_THRESHOLD = 25.0f; // FUEL CONSUMPTION at/below this isn't scarce enough to bother conserving, regardless of regen.
const float BOT_FUEL_REGEN_SCARCITY_RATIO = 0.25f; // regen below this fraction of consumption doesn't keep pace - fuel is a genuine net drain.
const float BOT_BOUNCE_CHANCE = 0.3f; // probability Bounce actually engages on an eligible decision window - keeps it an occasional technique rather than the automatic fallback whenever SeekHighGround fails.
const float BOT_CONSERVE_FUEL_CHANCE = 0.6f; // probability the whole fuel-conserving branch (SeekHighGround/Bounce) wins a movement decision when eligible, so combat/low-fuel-retreat still get regular turns instead of being starved for as long as there's somewhere to climb to.

// Open space (walls OFF): nothing stops a fall but the jetpack, and falling past
// the out-of-bounds line is elimination (#168).
const float BOT_BOUNDS_MARGIN_FRAC = 0.25f; // StayInBounds keeps the bot this fraction of halfSize inside the arena edge - far enough up that there are still platforms BELOW it to land on, since climbing back is what a scarce tank can't afford.
const float BOT_BRAKE_FUEL_RESERVE = 1.25f; // StayInBounds starts braking once the fuel it would take to stop the fall is within this factor of the tank.
const float BOT_DIVE_FUEL_FRACTION = 0.5f;  // earth-gravity dives are refused once the descent reaches this fraction of the speed the tank can take off a fall.
const float BOT_VELOCITY_DEADBAND = 1.0f;   // steerVelocity stops pushing once the velocity is within this many m/s of what it wants (no jitter around the target).
const float BOT_SAFE_LANDING_SPEED = 20.0f; // StayInBounds lets a bot drop onto a platform below this speed without braking (landing is free; much faster risks passing through a thin one).
const float BOT_HAVEN_CLIMB_COST = 4.0f;    // StayInBounds' landing pick: each metre a platform sits ABOVE the bot counts as this many metres of sideways travel (falling is free, climbing burns scarce fuel).
const float BOT_OPEN_SPACE_FUEL_RESERVE = 40.0f; // below this, only StayInBounds may use the jetpack - the rest of the tree walks, coasts, or waits on a platform; a bot StayInBounds landed also stays parked until it has this much.
const float BOT_JETPACK_RESUME_FUEL = 10.0f; // a bot that ran its tank dry keeps off the jetpack until it has regenerated this much (see BotController::drive).

// Climbing to higher ground when fuel is scarce (ClimbHigher).
const float BOT_HIGH_GROUND_LINE_FRAC = 0.0f; // a platform whose top is below this fraction of halfSize (0 = the arena's middle) is low ground a bot should climb out of.
const float BOT_CLIMB_MIN_GAIN = 10.0f;      // a climb target must be at least this much higher than the platform the bot is on.
const float BOT_CLIMB_MAX_GAIN_FRAC = 0.4f;  // ...and at most this fraction of halfSize higher.
const float BOT_CLIMB_MAX_REACH = 80.0f;     // ...and at most this far away horizontally.
const float BOT_CLIMB_DIST_WEIGHT = 0.5f;    // target pick: score = height gained - this * horizontal distance.
const float BOT_CLIMB_FUEL_MARGIN = 10.0f;   // a jetpack hop is only tried with this much fuel left over after it.
const float BOT_CLIMB_TIMEOUT = 30.0f;       // give up on a climb that has not launched after this many seconds - or, once launched, has not landed after this many more.
const float BOT_CLIMB_RETRY_SECONDS = 3.0f;  // after a failed climb, wait this long before trying again.
const float BOT_PUMP_MIN_GAIN = 1.5f;        // pump-bouncing is only used when each bounce multiplies the height by at least this much (e^2 * EARTH/MOON gravity).
const float BOT_PUMP_KICK_SPEED = 4.0f;      // a bot at rest starts a pump with a jetpack kick to this upward speed (~5 fuel).

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
        Vector3* outPerp = nullptr, float explosionRadius = EXPLOSION_DAMAGE_RADIUS) {
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
        if (len - along < explosionRadius) continue; // platform hugging `to`
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
    // Effective values under the match's OPTIONS sliders (GameSpace::speedBoost *
    // rocketSpeedScale, and explosionRadiusScale) - so lead-aim, line-of-sight
    // margins, and cover standoff stay accurate when a match scales rockets or
    // blasts up. Computed once in BotController::drive, not per-node.
    float rocketSpeed;
    float explosionRadius;
    // GameSpace::wallsEnabled - gates Bounce (no boundary to bounce off when
    // walls are disabled).
    bool  wallsEnabled;
    // GameSpace::fuelConsumptionRate / fuelRegenRate() - gates SeekHighGround
    // (only worth conserving fuel when consumption meaningfully outpaces regen).
    float fuelConsumptionRate;
    float fuelRegenRate;
};

template <typename TargetT>
class Node {
public:
    virtual ~Node() = default;
    virtual Status tick(Blackboard<TargetT>& bb) = 0;
};
//MARK: Horizontal drive
// Every movement node ends in out.moveAxis, and what moveAxis DOES depends on the
// match's COAST MODE (Player::updateVelocity):
//  - friction (coast OFF): horizontal velocity eases toward moveAxis * cruise
//    speed, and a zero moveAxis eases it to a stop. A direction is a complete
//    instruction, and "write nothing" means "stand still".
//  - coast (ON - the default): moveAxis only adds acceleration and nothing ever
//    takes speed away. Zero input keeps the bot drifting at whatever it has, and
//    a new heading is added ON TOP of the old velocity rather than replacing it.
// The nodes predate coast mode, so they state intent through these helpers:
// each one is the old friction code verbatim when coast is off, and the coast
// equivalent - steering the VELOCITY, not just the input - when it is on.

// World-space direction -> local moveAxis (its vertical part is ignored).
template <typename TargetT>
inline void setMoveDir(Blackboard<TargetT>& bb, Vector3 dir) {
    bb.out.moveAxis.y = Vector3DotProduct(dir, bb.bot.ForwardFlat());
    bb.out.moveAxis.x = Vector3DotProduct(dir, bb.bot.Right());
}

// Thrust along (wantVel - velocity), horizontally, so the velocity converges on
// wantVel. Correct in both modes; in coast it is the only way to turn or stop.
template <typename TargetT>
inline void steerVelocity(Blackboard<TargetT>& bb, Vector3 wantVel) {
    Vector3 err{wantVel.x - bb.bot.velocity.x, 0.0f, wantVel.z - bb.bot.velocity.z};
    if (Vector3Length(err) > BOT_VELOCITY_DEADBAND) setMoveDir(bb, Vector3Normalize(err));
    else bb.out.moveAxis = {0.0f, 0.0f};
}

// Stand still horizontally. Friction: no input, and friction stops the bot.
// Coast: brake against the drift - no input would carry it on forever.
template <typename TargetT>
inline void holdPosition(Blackboard<TargetT>& bb) {
    if (bb.bot.coastMode) steerVelocity(bb, Vector3{0.0f, 0.0f, 0.0f});
    else bb.out.moveAxis = {0.0f, 0.0f};
}

// Travel along `dir` (world space, need not be normalized or flat). Friction:
// moveAxis = dir, as the nodes always did. Coast: steer the velocity to dir's
// horizontal part at cruise speed, which also bleeds off the sideways drift a
// previous heading left behind (and a mostly-vertical dir asks for little
// horizontal speed, instead of a full-speed sideways push).
template <typename TargetT>
inline void driveToward(Blackboard<TargetT>& bb, Vector3 dir) {
    if (!bb.bot.coastMode) { setMoveDir(bb, dir); return; }
    float len = Vector3Length(dir);
    if (len < 1e-6f) { holdPosition(bb); return; }
    float cruise = bb.bot.speedWalk * bb.bot.speedBoost;
    steerVelocity(bb, Vector3{dir.x / len * cruise, 0.0f, dir.z / len * cruise});
}

// Come to a stop `standoff` short of `point` (horizontally): the speed that can
// still stop in the distance left, v = sqrt(2*a*d), capped at cruise. Used by
// every node in coast mode, and by StayInBounds in both modes (it cannot
// afford friction's overshoot above a platform).
template <typename TargetT>
inline void arriveAt(Blackboard<TargetT>& bb, Vector3 point, float standoff = 0.0f) {
    const Player& bot = bb.bot;
    Vector3 to{point.x - bot.position.x, 0.0f, point.z - bot.position.z};
    float d = Vector3Length(to);
    if (d < 1e-3f) { steerVelocity(bb, Vector3{0.0f, 0.0f, 0.0f}); return; }
    float accel = bot.accelerationWalk * bot.speedBoost;
    float speed = fminf(bot.speedWalk * bot.speedBoost, sqrtf(2.0f * accel * fmaxf(0.0f, d - standoff)));
    steerVelocity(bb, Vector3Scale(to, speed / d));
}

// Go to `point` and stop there. Friction: head straight for it and let friction
// do the stopping, as before. Coast: arriveAt, since nothing else will stop it.
template <typename TargetT>
inline void approach(Blackboard<TargetT>& bb, Vector3 point, float standoff = 0.0f) {
    if (bb.bot.coastMode) arriveAt(bb, point, standoff);
    else setMoveDir(bb, Vector3Normalize(Vector3Subtract(point, bb.bot.position)));
}

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
                                                          bb.allPlatforms, bb.bot.radius, &blockerPerp, bb.explosionRadius);
        if (!blocker) { holdPosition(bb); return Status::Success; } // clear line of sight — hold and snipe

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

        driveToward(bb, strafe);
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
        if (platformBlockingSegment(bb.bot.position, bb.target.position, bb.allPlatforms, bb.bot.radius, nullptr, bb.explosionRadius)) {
            holdPosition(bb);
            return Status::Success;
        }

        // Otherwise pick the closest platform and move to its far side from the target.
        const Platform* closest = nullptr;
        float best = std::numeric_limits<float>::max();
        for (const Platform& platform : bb.allPlatforms) {
            float d = Vector3Length(Vector3Subtract(platform.position, bb.bot.position));
            if (d < best) { closest = &platform; best = d; }
        }

        // Direction target->platform, continued past the platform to its far face; the
        // explosionRadius standoff keeps the bot clear of splash on the near face.
        Vector3 targetToPlat = Vector3Subtract(closest->position, bb.target.position);
        if (Vector3Length(targetToPlat) < 1e-3f) return Status::Failure; // platform ~on the target: useless as cover
        targetToPlat = Vector3Normalize(targetToPlat);
        Vector3 coveredPos = Vector3Add(closest->position, Vector3Scale(targetToPlat, bb.explosionRadius));

        approach(bb, coveredPos);
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
        // Close enough: stop closing in. In coast mode that takes braking, or
        // the bot would sail on into the target's face (or past it).
        if (dist < stopDist) { holdPosition(bb); return Status::Success; }
        // Friction mode runs in at full speed and friction's braking carries the
        // bot about v^2/2a further in once it crosses stopDist. Coast mode aims
        // for that same resting point - crossing stopDist still at speed, then
        // holdPosition brakes it - rather than creeping up on the stopDist
        // line and never quite crossing it, which left coasting bots sniping
        // from noticeably further out.
        float cruise = bb.bot.speedWalk * bb.bot.speedBoost;
        float brakeDist = cruise * cruise / (2.0f * bb.bot.accelerationWalk * bb.bot.speedBoost);
        approach(bb, bb.target.position, fmaxf(0.0f, stopDist - brakeDist));
        applyVerticalIntent(bb.bot.position.y, bb.target.position.y, bb.out);
        return Status::Running;
    }
};

//MARK: Move From Player
// Kites away from any player that is "bearing down" — within BOT_BEARING_DOWN_RANGE
// AND closing (velocity pointed at the bot) — weighted by proximity. Ignores
// opponents who are merely nearby but not advancing, so the bot doesn't flee a
// retreating enemy. Movement is wall-aware via steerAlongWalls (slides along a
// boundary rather than pressing into it).
template <typename TargetT>
class MoveFromPlayer : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        Vector3 totalAvoidance = {0, 0, 0};
        for (const Player& opponent : bb.allPlayers) {
            if (opponent.id == bb.bot.id) continue; // don't avoid yourself
            Vector3 away = Vector3Subtract(bb.bot.position, opponent.position);
            float dist = Vector3Length(away);
            if (dist <= 0.0f || dist >= BOT_BEARING_DOWN_RANGE) continue;         // not near
            Vector3 towardBot = Vector3Scale(away, -1.0f / dist);                 // opponent -> bot (unit)
            if (Vector3DotProduct(opponent.velocity, towardBot) <= 0.0f) continue; // not closing
            float weight = (BOT_BEARING_DOWN_RANGE - dist) / BOT_BEARING_DOWN_RANGE; // nearer = more urgent
            totalAvoidance = Vector3Add(totalAvoidance, Vector3Scale(Vector3Scale(away, 1.0f / dist), weight));
        }
        if (Vector3LengthSqr(totalAvoidance) <= 0.0f) return Status::Failure; // nobody bearing down — let the Selector fall through (Attack, cover, etc.)
        if (!steerAlongWalls(bb, Vector3Normalize(totalAvoidance))) return Status::Failure; // cornered on every escape axis
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
        if (vec3ApproxEqual(bb.bot.position, landingPosition, bb.bot.radius)) { holdPosition(bb); return Status::Success; } // on platform, done

        approach(bb, landingPosition);
        applyVerticalIntent(bb.bot.position.y, landingPosition.y, bb.out);
        return Status::Running;
    }
};

//MARK: Idle
template <typename TargetT>
class Idle : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        holdPosition(bb); // coast mode: brake, or "idle" is a drift off the platform
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

// Drives out.moveAxis (+ vertical intent) from a world-space escape direction,
// first zeroing any component pointing into a boundary the bot is already near so
// it slides along the wall instead of pressing into it. Returns false when every
// escape component is blocked (cornered). Shared by AvoidAsteroid and AvoidWall.
template <typename TargetT>
inline bool steerAlongWalls(Blackboard<TargetT>& bb, Vector3 dir) {
    float bound = bb.walls.halfSize - BOT_WALL_AVOID_BUFFER;
    Vector3 p = bb.bot.position;
    if ((dir.x < 0.0f && p.x < -bound) || (dir.x > 0.0f && p.x > bound)) dir.x = 0.0f;
    if ((dir.y < 0.0f && p.y < -bound) || (dir.y > 0.0f && p.y > bound)) dir.y = 0.0f;
    if ((dir.z < 0.0f && p.z < -bound) || (dir.z > 0.0f && p.z > bound)) dir.z = 0.0f;
    if (Vector3LengthSqr(dir) <= 1e-6f) return false;            // cornered on every escape axis
    dir = Vector3Normalize(dir);
    // In coast mode this also brakes the velocity along an axis zeroed above -
    // zeroing the INPUT alone would let the bot keep sliding into the wall.
    driveToward(bb, dir);
    // Scale the vertical target by halfSize so a normalized dir.y clears the
    // BOT_VERTICAL_THRESHOLD deadzone (|dir.y| > ~0.125 => thrust), reusing
    // applyVerticalIntent rather than re-implementing the jetpack/gravity choice.
    applyVerticalIntent(p.y, p.y + dir.y * bb.walls.halfSize, bb.out);
    return true;
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
        if (platformBlockingSegment(bb.bot.position, bb.target.position, bb.allPlatforms, bb.bot.radius, nullptr, bb.explosionRadius))
            return Status::Failure;
        // no ammo, can't fire
        if (bb.bot.ammo <= 0) return Status::Failure;
        // A bot with low health returns failure to the fireAtTarget selector, which will fall through to
        // firing asteroids instead of the player.  The threshold is scaled by the bot's aggression,
        // so a more aggressive bot will continue to fire at the player longer with lower health than a timid bot.
        if (bb.bot.health < BOT_LOW_HEALTH_THRESHOLD * (1.0f - 0.8f * bb.profile.aggression)) return Status::Failure;

        auto [isOnTarget, aimDir] = onTarget(bb.target, bb.bot, 0.0f, bb.rocketSpeed);
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
                if (platformBlockingSegment(bb.bot.position, asteroid.position, bb.allPlatforms, bb.bot.radius, nullptr, bb.explosionRadius)) continue;
                closestAsteroid = &asteroid;
                closestDist = dist;
            }
        }

        //find aiming scheme to lead the asteroid
        if (closestAsteroid) {
            auto [isOnTarget, aimDir] = onTarget(*closestAsteroid, bb.bot, 0.0f, bb.rocketSpeed);
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
        // Drive the body away from the threat (aim stays owned by fireAtTarget, so
        // the bot keeps shooting while it dodges), sliding along any wall rather
        // than pressing into it. Cornered on every escape axis -> fall through so
        // the attack selector reaches avoidWall, which peels the bot inward.
        Vector3 awayDir = Vector3Normalize(Vector3Negate(threatDir));
        if (!steerAlongWalls(bb, awayDir)) return Status::Failure;
        return Status::Running;
    }
};

//MARK: Avoid Wall
// Peels a bot off a boundary it has parked against. Timid bots that hold a
// sniping spot (FindLineOfSight writes no movement) decelerate to a dead stop; if
// that stop is at a wall, nothing pushes them back inward — this does. The
// departure isn't a straight line to center: it blends the inward wall normal
// with a latched lateral tangent so different bots/engagements leave at different
// angles ("light wander"), then releases (Failure) once the bot is clear by a
// hysteresis margin, handing control back to the attack path (find LOS / close in).
template <typename TargetT>
class AvoidWall : public Node<TargetT> {
    int stateId;
    // Inward directions from every wall the point is within `bound` of (a corner
    // yields two/three components). Zero vector = not near any wall.
    static Vector3 inwardNormal(Vector3 p, float bound) {
        Vector3 n{0, 0, 0};
        if (p.x >  bound) n.x = -1.0f; else if (p.x < -bound) n.x = 1.0f;
        if (p.y >  bound) n.y = -1.0f; else if (p.y < -bound) n.y = 1.0f;
        if (p.z >  bound) n.z = -1.0f; else if (p.z < -bound) n.z = 1.0f;
        return n;
    }
public:
    AvoidWall(int id) : stateId(id) {}
    Status tick(Blackboard<TargetT>& bb) override {
        BotDecision& s = bb.decisions[stateId]; // activeBranch: -1 idle / 1 engaged; interval: latched lateral bias
        Vector3 pos = bb.bot.position;
        float triggerBound = bb.walls.halfSize - BOT_WALL_AVOID_BUFFER;
        float releaseBound = triggerBound - BOT_WALL_CLEAR_MARGIN; // deeper inside => hysteresis

        // Coast mode: nothing slows a bot heading for a wall, so judge by where
        // it would come to a stop (v^2/2a further on), not where it is now - a
        // coasting bot covers the buffer in a fraction of a second. Friction
        // mode keeps judging by position, as before.
        Vector3 probe = pos;
        if (bb.bot.coastMode) {
            Vector3 hv{bb.bot.velocity.x, 0.0f, bb.bot.velocity.z};
            float accel = bb.bot.accelerationWalk * bb.bot.speedBoost;
            probe = Vector3Add(pos, Vector3Scale(hv, Vector3Length(hv) / (2.0f * accel)));
        }

        if (s.activeBranch < 0) {                       // idle: engage only when near a wall
            if (Vector3LengthSqr(inwardNormal(probe, triggerBound)) <= 0.0f) return Status::Failure;
            s.activeBranch = 1;
            s.interval = RandomFloat(-1.0f, 1.0f);      // latched lateral wander bias for this departure
        } else if (Vector3LengthSqr(inwardNormal(probe, releaseBound)) <= 0.0f) {
            s.activeBranch = -1;                         // cleared the wall by the margin: hand back to attack
            return Status::Failure;
        }

        Vector3 n = Vector3Normalize(inwardNormal(probe, releaseBound));
        Vector3 tan = Vector3CrossProduct(n, {0.0f, 1.0f, 0.0f});
        if (Vector3LengthSqr(tan) < 1e-4f) tan = Vector3CrossProduct(n, {1.0f, 0.0f, 0.0f}); // n ~ up (floor/ceiling)
        tan = Vector3Normalize(tan);
        Vector3 dir = Vector3Normalize(Vector3Add(n, Vector3Scale(tan, BOT_WALL_WANDER_STRENGTH * s.interval)));

        // The inward departure direction is never fully wall-blocked, so ignore the
        // cornered return; this also gives correct floor/ceiling lift-off.
        steerAlongWalls(bb, dir);
        return Status::Running;
    }
};

// Net upward acceleration of a full jetpack burn against moon gravity - how hard
// a bot can brake a fall. Reads the OPTIONS multipliers ApplyPlayerInput mirrors
// onto the player, so it follows JETPACK THRUST / SPEED BOOST.
inline float botJetpackBrake(const Player& bot) {
    return fmaxf(bot.accelerationJetpack * bot.speedBoost * bot.jetpackThrust - MOON_GRAVITY, 1.0f);
}
// Downward speed (m/s) the bot's remaining fuel can take off a fall.
inline float botFuelDeltaV(const Player& bot, float fuelConsumptionRate) {
    if (fuelConsumptionRate <= 0.0f) return std::numeric_limits<float>::max();
    return bot.fuel / fuelConsumptionRate * botJetpackBrake(bot);
}

// Standing on (or bouncing in place on) platform p: inside its footprint and
// within a small hop of its top. THE VOID's platforms are springy, so a bot
// settling onto one is rarely at rest on any given frame.
inline bool botOnPlatform(const Player& bot, const Platform& p) {
    float top = p.position.y + 0.5f * p.size.y + bot.radius;
    return fabsf(bot.position.x - p.position.x) < 0.5f * p.size.x &&
           fabsf(bot.position.z - p.position.z) < 0.5f * p.size.z &&
           bot.position.y > top - bot.radius && bot.position.y < top + 2.0f * BOT_VERTICAL_THRESHOLD;
}

//MARK: Stay In Bounds
// Open space only (walls OFF): nothing stops a fall but the jetpack, and there
// is no floor - past the out-of-bounds line a bot is eliminated (#168). At
// scarce regen a tank refilling in mid-air cannot even out-thrust gravity, so
// the one safe place to wait for fuel is standing on a platform.
//
// Engages while falling when either (a) a full burn started now would not stop
// the fall above the arena's lower margin (BOT_BOUNDS_MARGIN_FRAC), or (b) in
// the lower half, the fall is already close to all the tank can take off it;
// when (c) the bot is out past the side margin and still heading out; or when
// (d) it is standing on a platform in the lower half on a low tank. It then
// makes for a platform it can reach - one below if there is one, since falling
// is free and climbing is not - arriving rather than flying past, braking with
// the jetpack only as late as a full burn allows (never earth gravity). It lets
// go once the bot has landed or is rising back inside AND has refilled to
// BOT_OPEN_SPACE_FUEL_RESERVE; until then a landed bot stays parked.
//
// A hard override: BotController runs it OUTSIDE the movement latch, so a fall
// never waits out someone else's decision window.
template <typename TargetT>
class StayInBounds : public Node<TargetT> {
    int stateId;
public:
    StayInBounds(int id) : stateId(id) {}
    Status tick(Blackboard<TargetT>& bb) override {
        BotDecision& s = bb.decisions[stateId]; // activeBranch: -1 idle / 1 engaged
        if (bb.wallsEnabled) { s.activeBranch = -1; return Status::Failure; }

        const Player& bot = bb.bot;
        float half   = bb.walls.halfSize;
        float edge   = half * (1.0f - BOT_BOUNDS_MARGIN_FRAC);
        float vy     = bot.velocity.y;
        bool  falling = vy < -1.0f; // a bot resting on a platform reads ~0, not exactly 0
        float stopY  = falling ? bot.position.y - vy * vy / (2.0f * botJetpackBrake(bot)) : bot.position.y; // where a full burn from now would stop the fall
        // (b) only in the lower half: higher up there is still room to regenerate
        // on the way down, and braking there would just be a bot hovering.
        bool fallingOut = falling && (stopY < -edge ||
                          (bot.position.y < 0.0f && -vy * BOT_BRAKE_FUEL_RESERVE > botFuelDeltaV(bot, bb.fuelConsumptionRate)));
        Vector3 flatPos{bot.position.x, 0.0f, bot.position.z};
        Vector3 flatVel{bot.velocity.x, 0.0f, bot.velocity.z};
        bool driftingOut = (fabsf(bot.position.x) > edge || fabsf(bot.position.z) > edge) &&
                           Vector3DotProduct(flatPos, flatVel) > 0.0f;

        if (s.activeBranch < 0) {
            // (d) in the lower half, standing on a platform on a low tank: stay
            // put. Stepping off it there is a fall the tank cannot catch.
            bool parkLow = false;
            if (bot.position.y < 0.0f && bot.fuel < BOT_OPEN_SPACE_FUEL_RESERVE && fabsf(vy) < BOT_SAFE_LANDING_SPEED)
                for (const Platform& p : bb.allPlatforms)
                    if (botOnPlatform(bot, p)) { parkLow = true; break; }
            if (!fallingOut && !driftingOut && !parkLow) return Status::Failure;
            s.activeBranch = 1;
        }

        // The haven: a platform anywhere inside the arena, the cheapest to reach
        // - height ABOVE the bot counts BOT_HAVEN_CLIMB_COST times over. One the
        // bot cannot actually reach is only taken when nothing else is left:
        // above, that is stopping the fall and then rising that far on what is
        // in the tank; below, it is getting there across the gap before gravity
        // has taken it past the platform's height, with the tank buying some
        // hover time on the way. None at all -> the middle of the arena, at the
        // bot's own height.
        float inside   = half - BOT_WALL_AVOID_BUFFER;
        float fuelDv   = botFuelDeltaV(bot, bb.fuelConsumptionRate);
        float walk     = fmaxf(bot.speedWalk * bot.speedBoost, 1.0f);
        float down     = fmaxf(0.0f, -vy);
        // Hovering takes a MOON_GRAVITY-sized share of the jetpack's thrust, and
        // regen refunds some of that; a tank that out-regens the hover buys
        // unlimited time.
        float hoverBurn = bb.fuelConsumptionRate * MOON_GRAVITY / (botJetpackBrake(bot) + MOON_GRAVITY) - bb.fuelRegenRate;
        float hoverTime = hoverBurn > 0.0f ? bot.fuel / hoverBurn : std::numeric_limits<float>::max();
        const Platform* haven = nullptr;
        float bestCost = std::numeric_limits<float>::max();
        for (const Platform& p : bb.allPlatforms) {
            float top = p.position.y + 0.5f * p.size.y + bot.radius;
            if (top < -inside || fabsf(p.position.x) > inside || fabsf(p.position.z) > inside) continue;
            float across = hypotf(p.position.x - bot.position.x, p.position.z - bot.position.z);
            float climb  = fmaxf(0.0f, top - bot.position.y);
            float cost   = across + BOT_HAVEN_CLIMB_COST * climb;
            bool  reachable;
            if (climb > 0.0f) {
                reachable = down + sqrtf(2.0f * MOON_GRAVITY * climb) <= fuelDv;
            } else {
                float t = fmaxf(0.0f, across / walk - hoverTime); // unpowered part of the trip
                reachable = down * t + 0.5f * MOON_GRAVITY * t * t <= bot.position.y - top;
            }
            if (!reachable) cost += 1e6f; // last resort only
            if (cost < bestCost) { bestCost = cost; haven = &p; }
        }
        Vector3 goal = haven ? Vector3{haven->position.x, haven->position.y + 0.5f * haven->size.y + bot.radius, haven->position.z}
                             : Vector3{0.0f, bot.position.y, 0.0f};

        // Let go once it is safe - rising back inside, or landed on the haven -
        // AND has enough in the tank to be worth moving again. Landed short of
        // that, stay parked (the rest of the tree would walk it off the edge on
        // an empty tank): centred, no jetpack, waiting on regen. A bounce off a
        // springy platform is "rising" too, which is why the fuel test covers
        // both.
        float clear = edge - BOT_WALL_CLEAR_MARGIN;
        bool overHaven = haven && fabsf(bot.position.x - haven->position.x) < 0.5f * haven->size.x &&
                                  fabsf(bot.position.z - haven->position.z) < 0.5f * haven->size.z &&
                                  bot.position.y > goal.y - bot.radius;
        bool landed = haven && botOnPlatform(bot, *haven) && fabsf(vy) < BOT_SAFE_LANDING_SPEED;
        bool rising = vy >= 0.0f && bot.position.y > -clear &&
                      fabsf(bot.position.x) < clear && fabsf(bot.position.z) < clear;
        if ((rising || landed) && bot.fuel >= BOT_OPEN_SPACE_FUEL_RESERVE) { s.activeBranch = -1; return Status::Failure; }

        // Arrive, don't fly past - in BOTH modes: friction's overshoot is enough
        // to miss a 16 m platform on the way down.
        arriveAt(bb, goal);
        // Climb while below the goal. Above it, coast down: over the platform,
        // hitting it is the brake (it costs nothing) unless the fall is fast
        // enough to risk passing through; short of it, brake only once a full
        // burn is what it takes to arrive rather than overshoot - fuel spent on
        // stopping, not on hovering.
        if (landed)                      bb.out.jetpack = false;
        else if (bot.position.y < goal.y) bb.out.jetpack = vy < BOT_VERTICAL_THRESHOLD;
        else if (overHaven)              bb.out.jetpack = -vy > BOT_SAFE_LANDING_SPEED && stopY < goal.y + BOT_VERTICAL_THRESHOLD;
        else                             bb.out.jetpack = stopY < goal.y + BOT_VERTICAL_THRESHOLD;
        bb.out.earthGravity = false;
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

// Fuel is only worth conserving/hunting for when both (a) FUEL CONSUMPTION
// (OPTIONS slider) meaningfully outpaces the default drain, and (b) FUEL
// REGEN isn't already keeping pace with it — a net drain, not just a high
// number on both sides. Shared by NeedsBonus, SeekHighGround, and Bounce so
// "scarce" means one thing everywhere instead of three separately-tuned checks.
template <typename TargetT>
inline bool fuelIsScarce(const Blackboard<TargetT>& bb) {
    return bb.fuelConsumptionRate > BOT_FUEL_SCARCITY_THRESHOLD &&
           bb.fuelRegenRate < bb.fuelConsumptionRate * BOT_FUEL_REGEN_SCARCITY_RATIO;
}

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
            bb.bot.health < PLAYER_MAX_HEALTH - (75.0f - (ASTEROID_HEALTH_AWARD * bb.profile.aggression)) ||
            bb.bot.ammo   < PLAYER_MAX_AMMO   - 80;
        if (fuelIsScarce(bb) && bb.bot.fuel < BOT_LOW_FUEL_THRESHOLD) needsBonus = true;
        return needsBonus ? Status::Success : Status::Failure;
    }
};

//MARK: Climb Higher
// The mandate to get off low ground while fuel is scarce. Standing on a platform
// below BOT_HIGH_GROUND_LINE_FRAC, a bot picks a higher platform within reach and
// works its way up to it, then does it again from there - instead of waiting
// out the whole match on the first low platform it landed on.
//
// Two ways up:
//  - PUMP (costs nothing): dive toward the platform under EARTH gravity and drop
//    back to moon gravity a frame or two before contact (earth gravity passes
//    through platforms - see collisions.cpp). The bounce returns e * the impact
//    speed and the rise is under moon gravity, so each bounce multiplies the
//    height by e^2 * EARTH/MOON: x4.9 at THE VOID's 0.9, a LOSS at the default
//    0.33 - hence BOT_PUMP_MIN_GAIN. A bot at rest needs something to multiply,
//    so it starts with a small jetpack kick; one landing with a bounce already
//    in it needs none.
//  - HOP: a jetpack burn, when the tank can pay for it with a margin to spare.
// Neither available: in open space, park on the platform while the tank
// refills (stepping off low down is a fall the tank cannot catch); with walls,
// leave it to the rest of the tree.
//
// Once the bounce or burn will carry the bot high enough to cross to the
// target in the air, it is LAUNCHED: it steers across and lands. Landing on the
// target is success; falling past it, off the platform, or timing out is
// failure, and StayInBounds (next in line) catches any fall.
//
// State: decisions[climbId]: activeBranch = target platform index (-1 idle),
// timer = seconds into this climb, interval = launched (1) or not (0).
// decisions[baseId]: activeBranch = the platform climbed FROM, interval = the
// apex the climb needs, timer = retry cooldown while idle / hop-burn-in-progress
// (> 0) while climbing.
template <typename TargetT>
class ClimbHigher : public Node<TargetT> {
    int climbId, baseId;

    static float topOf(const Platform& p, const Player& bot) { return p.position.y + 0.5f * p.size.y + bot.radius; }

    // Seconds to travel d horizontally from rest and stop (arriveAt's profile).
    static float travelTime(float d, float accel, float vmax) {
        if (d <= vmax * vmax / accel) return 2.0f * sqrtf(d / accel);
        return d / vmax + vmax / accel;
    }
    // Lowest apex from which a bot launched off baseTop stays above targetTop
    // long enough to cross `across` metres.
    static float neededApex(float baseTop, float targetTop, float across, const Player& bot) {
        float need = 1.25f * travelTime(across, bot.accelerationWalk * bot.speedBoost,
                                        bot.speedWalk * bot.speedBoost) + 0.3f;
        float apex = targetTop + 2.0f;
        for (int k = 0; k < 200; ++k, apex += 2.0f) {
            float v = sqrtf(2.0f * MOON_GRAVITY * (apex - baseTop));
            float t = v / MOON_GRAVITY + sqrtf(2.0f * (apex - targetTop) / MOON_GRAVITY);
            if (t >= need) break;
        }
        return apex;
    }
    // Fuel a jetpack burn from rest takes to reach an apex `rise` metres up:
    // burn at the net thrust A for t, then coast; rise = A t^2/2 * (1 + A/g).
    static float hopFuel(float rise, const Player& bot, float consumption) {
        float a = botJetpackBrake(bot);
        return consumption * sqrtf(2.0f * fmaxf(rise, 0.0f) / (a * (1.0f + a / MOON_GRAVITY)));
    }
    static bool pumpable(const Platform& p) {
        return p.isBouncy && p.elasticityPlayer * p.elasticityPlayer * EARTH_GRAVITY / MOON_GRAVITY >= BOT_PUMP_MIN_GAIN;
    }
    // Can a hop off `base` reach `apex` and leave `keep` in the tank? (The
    // vertical jetpack speed is capped, so some rises are out of reach at any fuel.)
    static bool hopAffordable(const Platform& base, float apex, const Player& bot, float consumption,
                              float keep = BOT_CLIMB_FUEL_MARGIN) {
        float rise = apex - topOf(base, bot);
        float vCap = bot.speedJetpack * bot.speedBoost * bot.jetpackThrust;
        if (2.0f * MOON_GRAVITY * rise > 0.8f * vCap * vCap) return false;
        return bot.fuel >= hopFuel(rise, bot, consumption) + keep;
    }
    Status stop(BotDecision& c, BotDecision& b, float cooldown) {
        c.activeBranch = -1; c.interval = 0.0f; c.timer = 0.0f;
        b.timer = cooldown;
        return Status::Failure;
    }

public:
    ClimbHigher(int climb, int base) : climbId(climb), baseId(base) {}
    Status tick(Blackboard<TargetT>& bb) override {
        BotDecision& c = bb.decisions[climbId];
        BotDecision& b = bb.decisions[baseId];
        const Player& bot = bb.bot;
        const std::vector<Platform>& plats = bb.allPlatforms;
        float vy = bot.velocity.y;

        if (c.activeBranch < 0) {
            if (b.timer > 0.0f) { b.timer -= bb.dt; return Status::Failure; } // retry cooldown
            if (!fuelIsScarce(bb) || plats.empty() || fabsf(vy) >= BOT_SAFE_LANDING_SPEED) return Status::Failure;
            // Standing (or bouncing) on low ground?
            int base = -1;
            for (int i = 0; i < (int)plats.size(); ++i)
                if (botOnPlatform(bot, plats[i])) { base = i; break; }
            float half = bb.walls.halfSize;
            if (base < 0 || plats[base].position.y >= BOT_HIGH_GROUND_LINE_FRAC * half) return Status::Failure;

            // The best rung up: height gained, less a charge for distance.
            const Platform& P = plats[base];
            float baseTop = topOf(P, bot), inside = half - BOT_WALL_AVOID_BUFFER;
            int target = -1; float bestScore = -std::numeric_limits<float>::max(), bestApex = 0.0f;
            for (int i = 0; i < (int)plats.size(); ++i) {
                const Platform& T = plats[i];
                float gain = topOf(T, bot) - baseTop;
                float across = hypotf(T.position.x - P.position.x, T.position.z - P.position.z);
                if (gain < BOT_CLIMB_MIN_GAIN || gain > BOT_CLIMB_MAX_GAIN_FRAC * half || across > BOT_CLIMB_MAX_REACH) continue;
                if (fabsf(T.position.x) > inside || fabsf(T.position.z) > inside || topOf(T, bot) > inside) continue;
                float apex = neededApex(baseTop, topOf(T, bot), across, bot);
                if (!pumpable(P) && !hopAffordable(P, apex, bot, bb.fuelConsumptionRate) && bb.wallsEnabled) continue;
                float score = gain - BOT_CLIMB_DIST_WEIGHT * across;
                if (score > bestScore) { bestScore = score; target = i; bestApex = apex; }
            }
            if (target < 0) return Status::Failure;
            c.activeBranch = target; c.timer = 0.0f; c.interval = 0.0f;
            b.activeBranch = base;   b.interval = bestApex; b.timer = 0.0f;
        }

        if (c.activeBranch >= (int)plats.size() || b.activeBranch < 0 || b.activeBranch >= (int)plats.size())
            return stop(c, b, 0.0f);
        const Platform& T = plats[c.activeBranch];
        const Platform& P = plats[b.activeBranch];
        float targetTop = topOf(T, bot), baseTop = topOf(P, bot), apexNeeded = b.interval;
        c.timer += bb.dt;
        bb.out.jetpack = false;
        bb.out.earthGravity = false;

        if (botOnPlatform(bot, T) && vy <= 0.0f) return stop(c, b, 0.0f);                 // made it - chain from here next tick
        if (b.timer > 0.0f && vy <= 0.0f && botOnPlatform(bot, P)) b.timer = 0.0f;         // a hop that fizzled is back on P
        if (c.timer > BOT_CLIMB_TIMEOUT) return stop(c, b, BOT_CLIMB_RETRY_SECONDS);

        bool launched = c.interval > 0.0f;
        if (!launched && vy > 0.0f && bot.position.y + vy * vy / (2.0f * MOON_GRAVITY) >= apexNeeded - 1.0f)
            launched = true, c.interval = 1.0f, c.timer = 0.0f; // the flight gets its own timeout

        if (launched) {
            // Fell past it - only once coming back DOWN: a launch starts well
            // below the target (it is the bounce off the platform underneath).
            if (vy < 0.0f && bot.position.y < targetTop - 2.0f * bot.radius) return stop(c, b, BOT_CLIMB_RETRY_SECONDS);
            arriveAt(bb, Vector3{T.position.x, targetTop, T.position.z});
            return Status::Running;
        }

        // Still working up off the base platform: stay over it.
        bool overBase = fabsf(bot.position.x - P.position.x) < 0.5f * P.size.x &&
                        fabsf(bot.position.z - P.position.z) < 0.5f * P.size.z;
        if (bot.position.y < baseTop - 2.0f * bot.radius) return stop(c, b, BOT_CLIMB_RETRY_SECONDS);     // fell off it
        arriveAt(bb, Vector3{P.position.x, baseTop, P.position.z});

        // Which way up, re-judged each tick (a pump never spends fuel, so the tank
        // only grows while it runs): a hop that still leaves the usual reserve
        // is fastest; otherwise a free pump; otherwise a hop the tank just
        // covers. A hop, once burning, burns until the launch test above says it
        // will carry (or the tank runs dry and the bot drops back onto P).
        bool hopNow = b.timer > 0.0f ||
                      hopAffordable(P, apexNeeded, bot, bb.fuelConsumptionRate, BOT_OPEN_SPACE_FUEL_RESERVE) ||
                      (!pumpable(P) && hopAffordable(P, apexNeeded, bot, bb.fuelConsumptionRate));
        if (hopNow) {
            b.timer = 1.0f;
            bb.out.jetpack = true;
            return Status::Running;
        }
        if (pumpable(P)) {
            float e = P.elasticityPlayer;
            float above = bot.position.y - baseTop;
            if (vy < 0.0f && overBase) {
                // Descending: dive while a moon-gravity finish would not bounce
                // high enough, and let go of earth gravity just before contact.
                float moonApex = baseTop + e * e * (vy * vy + 2.0f * MOON_GRAVITY * fmaxf(above, 0.0f)) / (2.0f * MOON_GRAVITY);
                float contactSoon = -vy * 2.0f * bb.dt + 0.25f;
                bb.out.earthGravity = moonApex < apexNeeded && above > contactSoon;
            } else if (vy >= 0.0f && above < 0.5f && vy < BOT_PUMP_KICK_SPEED && bot.fuel > 1.0f) {
                bb.out.jetpack = true; // at rest: kick off the first bounce
            }
            return Status::Running;
        }
        if (bb.wallsEnabled) return stop(c, b, BOT_CLIMB_RETRY_SECONDS); // walls: nothing to wait for here
        return Status::Running;        // open space: parked, waiting for the tank to afford the hop
    }
};

// MARK: Conserve Fuel Movement Types
// MARK: Seek High Ground
template <typename TargetT>
class SeekHighGround : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        // Only worth conserving fuel when it's actually scarce - fly freely
        // otherwise, there's nothing to conserve for.
        if (!fuelIsScarce(bb)) return Status::Failure;

        // in a fuel concious mode, the bot should aim to use the jetpack as little as possible
        // by landing on platforms or coasting.  This movement type is used to move the bot towards
        // the nearest higher platform.

        // Keep climbing toward a still-higher platform as long as there's fuel to
        // spare - not just until some fixed height. Each arrival re-scans for
        // something higher than the NEW position (see the `platform.position.y <=
        // bb.bot.position.y` filter below), so a bot chains progressively higher
        // platforms until either fuel drops below the threshold (falls to the
        // coast branch) or nothing higher is left (`!closestPlatform` -> Failure,
        // handing control back to the rest of the tree instead of squatting).
        if (bb.bot.fuel > BOT_LOW_FUEL_THRESHOLD) {
            // a bot with fuel to spare seeks to go higher still.
            
            // find nearest platform that is higher.
            if (bb.allPlatforms.empty()) return Status::Failure;

            // find the best platform that is higher than the bot's current position,
            // scoring by a blend of velocity alignment and proximity.
            // alignment = dot(normVel, normDir) ∈ [-1,1]: +1 means already heading there.
            // proximity = 1/(1+dist): bounded (0,1], closer scores higher.
            // score = ALIGN_WEIGHT * alignment + (1-ALIGN_WEIGHT) * proximity.
            // A single score lets well-aligned farther platforms beat nearby ones
            // that would require a sharp course change, and vice-versa.
            const float ALIGN_WEIGHT = 0.7f;
            const Platform* closestPlatform = nullptr;
            float bestScore = -std::numeric_limits<float>::max();
            const float velLen = Vector3Length(bb.bot.velocity);
            const Vector3 normVel = (velLen > 0.01f)
                ? Vector3Scale(bb.bot.velocity, 1.0f / velLen)
                : Vector3{}; // no velocity — alignment term contributes 0, pure proximity wins
            for (const Platform& platform : bb.allPlatforms) {
                if (platform.position.y <= bb.bot.position.y) continue; // must be above
                Vector3 dir = Vector3Subtract(platform.position, bb.bot.position);
                float dist = Vector3Length(dir);
                if (dist < 0.001f) continue;
                Vector3 normDir = Vector3Scale(dir, 1.0f / dist);
                float alignment = Vector3DotProduct(normVel, normDir); // cos of angle to platform
                float proximity = 1.0f / (1.0f + dist);
                float score = ALIGN_WEIGHT * alignment + (1.0f - ALIGN_WEIGHT) * proximity;
                if (score > bestScore) {
                    bestScore = score;
                    closestPlatform = &platform;
                }
            }
            if (!closestPlatform) return Status::Failure;

            // target a landing point one bot-radius above the platform surface
            Vector3 landingPosition = closestPlatform->position;
            landingPosition.y += (bb.bot.radius * 2.0f);
            // becuase of the threhold of equality, all points will be above the platform surface.
            // On success, the bot will move to Idle and fall to the platform surface.
            // might need to add something to counteract the bot's momentum if it is also moving horizontally.

            // considered successful if the bot is at the target point (within its radius).  Third arg is the threshold for equality.
            if (vec3ApproxEqual(bb.bot.position, landingPosition, bb.bot.radius)) { holdPosition(bb); return Status::Success; } // on platform, done

            approach(bb, landingPosition);
            applyVerticalIntent(bb.bot.position.y, landingPosition.y, bb.out);
            return Status::Running;
        }
        else {
            // low on fuel: can't afford more jetpack thrust, so glide (no jetpack)
            // to whatever platform is reachable under current momentum instead of
            // free-falling. If that's the platform already underfoot, this just
            // holds position - the intended "rest and let fuel regen" state.

            // coast to nearest platform that is reachable under current velocity
            if (bb.allPlatforms.empty()) return Status::Failure;

            // Ballistic apex under current vy with no jetpack (MOON_GRAVITY only).
            // If the bot is already falling (vy <= 0) the apex is its current height.
            const float vy = bb.bot.velocity.y;
            const float maxCoastY = (vy > 0.0f)
                ? bb.bot.position.y + (vy * vy) / (2.0f * MOON_GRAVITY)
                : bb.bot.position.y;

            // Find nearest platform whose landing surface sits at or below the apex,
            // measured by horizontal distance so tall platforms aren't unfairly penalised.
            const Platform* coastPlatform = nullptr;
            float coastDist = std::numeric_limits<float>::max();
            for (const Platform& platform : bb.allPlatforms) {
                // Landing surface: same convention as the jetpack branch above
                float landY = platform.position.y + (bb.bot.radius * 2.0f);
                if (landY > maxCoastY) continue; // unreachable without upward accel
                Vector3 diff = Vector3Subtract(platform.position, bb.bot.position);
                diff.y = 0.0f;
                float horizDist = Vector3Length(diff);
                if (horizDist < coastDist) {
                    coastDist = horizDist;
                    coastPlatform = &platform;
                }
            }
            if (!coastPlatform) return Status::Failure;

            Vector3 landingPosition = coastPlatform->position;
            landingPosition.y += (bb.bot.radius * 2.0f);

            if (vec3ApproxEqual(bb.bot.position, landingPosition, bb.bot.radius)) { holdPosition(bb); return Status::Success; }

            approach(bb, landingPosition);
            // No jetpack — coast only. Apply earth gravity if we need to descend faster.
            if (landingPosition.y < bb.bot.position.y - BOT_VERTICAL_THRESHOLD)
                bb.out.earthGravity = true;
            return Status::Running;
        }
    }
};

// MARK: bounce
template <typename TargetT>
class Bounce : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        // Same fuel-scarcity gate as SeekHighGround: with fuel abundant, there's
        // nothing worth conserving, so don't drop for the bottom wall at all.
        if (!fuelIsScarce(bb)) return Status::Failure;
        // If boundary walls are disabled, or the wall is too dead to bounce off,
        // pass through to the next node in the selector.
        if (!bb.wallsEnabled || bb.walls.elasticityPlayer < 0.2f) return Status::Failure;

        if (bb.bot.position.y > -(bb.walls.halfSize - BOT_WALL_AVOID_BUFFER)) {
            // bot drops via earth grav to bounce off the bottom wall - straight
            // down: with friction that is what no input means; in coast mode it
            // takes braking, or the drop becomes a slide into a side wall.
            holdPosition(bb);
            bb.out.earthGravity = true;
            return Status::Running;
        } else {
            // Bot has dropped enough - done. Return Failure (not Success): this
            // node writes no movement of its own here, so under Selector/
            // LatchedSelector semantics a Success here with zero movement output
            // would just get re-picked with nothing to show for it. Failure lets
            // the parent fall through to whatever's next (attack, etc).
            bb.out.earthGravity = false;
            return Status::Failure;
        }
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
    float (*probFn)(Blackboard<TargetT>&) = nullptr; // per-bot probability; overrides `probability` when set
public:
    Chance(int id, float p, Node<TargetT>* c) : stateId(id), probability(p), child(c) {}
    // Per-bot variant: the open probability is computed from the blackboard each
    // roll (e.g. scaled by bb.profile), since the shared node can't bake it in.
    Chance(int id, float (*p)(Blackboard<TargetT>&), Node<TargetT>* c)
        : stateId(id), probability(0.0f), child(c), probFn(p) {}
    Status tick(Blackboard<TargetT>& bb) override {
        BotDecision& s = bb.decisions[stateId];
        s.timer += bb.dt;
        if (s.activeBranch < 0 || s.timer >= s.interval) {         // (re)roll open/closed
            s.timer = (s.activeBranch < 0) ? 0.0f : s.timer - s.interval;
            s.interval = BOT_TICK_TIME * RandomFloat(BOT_TICK_JITTER_MIN, BOT_TICK_JITTER_MAX);
            float p = probFn ? probFn(bb) : probability;           // per-bot probability when a fn is supplied
            s.activeBranch = (RandomFloat(0.0f, 1.0f) < p) ? 1 : 0; // 1=open, 0=closed
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
// same destination as a human's PollLocalInput().
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
                    const BotProfile& profile,
                    float rocketSpeed = ROCKET_SPEED,
                    float explosionRadius = EXPLOSION_DAMAGE_RADIUS,
                    bool  wallsEnabled = WALLS_ENABLED,
                    float fuelConsumptionRate = FUEL_CONSUMPTION_RATE,
                    float fuelRegenRate = FUEL_CONSUMPTION_RATE * FUEL_REGEN_PCT_DEFAULT / 100.0f) {
    PlayerInput out;
    Blackboard<TargetT> bb{bot, target, allPlayers, platforms, asteroids, walls, out, dt, decisions, profile,
                           rocketSpeed, explosionRadius, wallsEnabled, fuelConsumptionRate, fuelRegenRate};
    tree.tick(bb);
    return out;
}

