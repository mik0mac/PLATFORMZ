#pragma once

#include "raylib.h"
#include "raymath.h"
#include "gamespace.h" // GameSpace, Player, Rocket
#include "input.h"     // PlayerInput, ApplyPlayerInput — bots feed the same path as humans
#include "constants.h"
#include "random.h"    // RandomFloat — per-frame aim jitter

const float BOT_TICK_TIME = 1.0f;          // seconds between bot decision ticks.
const float BOT_ATTACK_DISTANCE = 40.0f;   // distance to which bots move when attacking.
const float BOT_ASTEROID_ATTACK_BUFFER = 20.0f; // don't fire at asteroids closer than this distance, to avoid self-damage.
const float BOT_RETREAT_DISTANCE = 100.0f; // distance to which bots will retreat.
const float BOT_VERTICAL_THRESHOLD = 5.0f; // y-delta below which vertical corrections are ignored.
const float BOT_LOW_FUEL_THRESHOLD = 20.0f; // fuel level below which bots will seek a platform to land and regen.
const float BOT_LOW_HEALTH_THRESHOLD = 30.0f; // health level below which bots will retreat.

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

// Per-bot decision state for LatchedSelector. Lives outside the (shared) tree
// nodes so each bot keeps its own branch latch and timer — see LatchedSelector.
struct BotDecision {
    float timer = 0.0f;         // seconds accumulated since the last branch decision
    float interval = BOT_TICK_TIME; // this decision's jittered period (set on each re-decide)
    int   activeBranch = -1;    // index of the latched Selector child (-1 = decide now)
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
    PlayerInput& out;
    float dt;
    BotDecision& decision;      // per-bot branch latch/timer for LatchedSelector
    const BotProfile& profile;  // per-bot personality (aggression/accuracy)
};

template <typename TargetT>
class Node {
public:
    virtual ~Node() = default;
    virtual Status tick(Blackboard<TargetT>& bb) = 0;
};

//MARK: Action Nodes
// Writes out.moveAxis (local strafe/forward, not world space) — see
// Player::updateVelocity for the axes this is projected onto.
template <typename TargetT>
class MoveToTarget : public Node<TargetT> {
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

// Steers away from any player within BOT_RETREAT_DISTANCE, weighted by
// proximity. Writes out.moveAxis like MoveToTarget — same local-space rule.
template <typename TargetT>
class MoveToSafety : public Node<TargetT> {
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
        // Determine the Bot's next move to get to safety.
        Vector3 dir = Vector3Normalize(totalAvoidance);
        Vector3 fwd = bb.bot.ForwardFlat();
        Vector3 right = bb.bot.Right();
        bb.out.moveAxis.y = Vector3DotProduct(dir, fwd);
        bb.out.moveAxis.x = Vector3DotProduct(dir, right);
        // When evading, prefer gaining altitude over the threat rather than
        // descending into them — only apply earthGravity if avoidance already
        // points meaningfully downward.
        applyVerticalIntent(bb.bot.position.y, bb.bot.position.y + totalAvoidance.y, bb.out);
        return Status::Running;
    }
};

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

// Steers the bot's yaw one clamped step toward aimDir, writing out.lookDelta.
// Adds a personality-scaled random spread so low-accuracy bots don't track
// perfectly (accuracy 1 -> no spread, accuracy 0 -> +/-BOT_MAX_AIM_SPREAD).
// Shared by FireAtTarget and AttackAsteroid.
template <typename TargetT>
inline void steerYawToward(Blackboard<TargetT>& bb, Vector3 aimDir) {
    float desiredYaw = atan2f(aimDir.z, aimDir.x);
    desiredYaw += RandomFloat(-1.0f, 1.0f) * BOT_MAX_AIM_SPREAD * (1.0f - bb.profile.accuracy);
    float yawError = desiredYaw - bb.bot.yaw;
    while (yawError > PI) yawError -= 2.0f * PI;
    while (yawError < -PI) yawError += 2.0f * PI;
    float maxStep = BOT_TURN_RATE * bb.dt;
    float yawStep = Clamp(yawError, -maxStep, maxStep);
    bb.out.lookDelta.x = yawStep / bb.bot.lookSensitivity;
}

// Writes out.lookDelta (steers yaw toward the lead-aim solution) and
// out.fire once aligned.
template <typename TargetT>
class FireAtTarget : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        auto [isOnTarget, aimDir] = onTarget(bb.target, bb.bot);
        steerYawToward(bb, aimDir);

        if (isOnTarget) {
            bb.out.fire = true;
            return Status::Success;
        }
        return Status::Running;
    }
};

template <typename TargetT>
class AttackAsteroid : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        if (bb.allAsteroids.empty()) return Status::Failure;

        // find the closest asteroid that is not within the bot's buffer.
        // Aggressive bots accept a smaller safety buffer (riskier close shots).
        float buffer = BOT_ASTEROID_ATTACK_BUFFER * (1.0f - 0.5f * bb.profile.aggression);
        const Asteroid* closestAsteroid = nullptr;
        float closestDist = std::numeric_limits<float>::max();
        for (const Asteroid& asteroid : bb.allAsteroids) {
            float dist = Vector3Length(Vector3Subtract(asteroid.position, bb.bot.position));
            if (dist < closestDist && dist > buffer) {
                closestAsteroid = &asteroid;
                closestDist = dist;
            }
        }

        //find aiming scheme to lead the asteroid
        if (closestAsteroid) {
            auto [isOnTarget, aimDir] = onTarget(*closestAsteroid, bb.bot);
            steerYawToward(bb, aimDir);
            if (isOnTarget) {
                bb.out.fire = true;
                return Status::Success; // firing!
            }
            return Status::Running; // aiming solution exists, but not yet aligned.
        }
        return Status::Failure;  // no valid asteroid to attack
    }
};

template <typename TargetT>
class IsLowFuel : public Node<TargetT> {
public:
    Status tick(Blackboard<TargetT>& bb) override {
        return bb.bot.fuel < BOT_LOW_FUEL_THRESHOLD ? Status::Success : Status::Failure;
    }
};

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
// choice is frozen (~1s). The latch lives per-bot in bb.decision (not in this
// shared node), so one tree instance drives every bot independently.
template <typename TargetT>
class LatchedSelector : public Node<TargetT> {
    std::vector<Node<TargetT>*> children;
public:
    LatchedSelector(std::vector<Node<TargetT>*> kids) : children(std::move(kids)) {}
    Status tick(Blackboard<TargetT>& bb) override {
        BotDecision& d = bb.decision;
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
                    Node<TargetT>& tree, float dt,
                    BotDecision& decision,
                    const BotProfile& profile) {
    PlayerInput out;
    Blackboard<TargetT> bb{bot, target, allPlayers, platforms, asteroids, out, dt, decision, profile};
    tree.tick(bb);
    return out;
}

// /// Instantiate leaf nodes
// IsLowFuel<Player>      isLowFuel;
// IsLowHealth<Player>    isLowHealth;
// MoveToTarget<Player>   moveToTarget;
// MoveToSafety<Player>   moveToSafety;
// MoveToPlatform<Player> moveToPlatform;
// FireAtTarget<Player>   fireAtTarget;
// AttackAsteroid<Player> attackAsteroid;
// Idle<Player>           idle;

// // Compose the tree. `movement` is a LatchedSelector so the branch CHOICE is
// // re-evaluated only every BOT_TICK_TIME; the chosen action still ticks every
// // frame. fireAtTarget sits in the top Parallel, so aim/fire stay per-frame.
// Parallel<Player>        evadeAndShootAsteroid({ &moveToSafety, &attackAsteroid });
// Sequence<Player>        lowHealthResponse({ &isLowHealth, &evadeAndShootAsteroid });
// Sequence<Player>        lowFuelResponse({ &isLowFuel, &moveToPlatform });
// LatchedSelector<Player> movement({ &lowHealthResponse, &lowFuelResponse, &moveToTarget, &idle });
// Parallel<Player>        botTree({ &movement, &fireAtTarget });

// // Per bot, per frame (botDecisions[i-1]/botProfiles[i-1] are this bot's state):
// PlayerInput botIn = botInput(players[i], players[0], players,
//                              gameSpace.getPlatforms(), gameSpace.getAsteroids(),
//                              botTree, dt, botDecisions[i - 1], botProfiles[i - 1]);
// float gravity = botIn.earthGravity ? EARTH_GRAVITY : MOON_GRAVITY;
// ApplyPlayerInput(players[i], botIn, dt, gravity, gameSpace);