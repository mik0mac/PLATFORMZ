// C++ header file with the classes and functions related to the game logic.  Using raylib 3d.

#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>
#include <string>
#include <algorithm> // std::max/min over initializer lists (out-of-bounds check)

#include "random.h"
#include "constants.h"


//MARK: Walls
// The play-space boundary cube, centered on origin and spanning
// -halfSize..+halfSize on each axis. Treated as a first-class game element
// (owns its own state + appearance) like Platform/Asteroid/etc., rather than
// being loose fields on GameSpace. Collision response against the walls lives
// in collisions.cpp (next to detection), reading these values via getWalls().
class Walls {
public:
    // Centered on origin; cube spans -halfSize..+halfSize on each axis.
    float halfSize = GAMESPACE_HALF_SIZE;
    float gridSpacing = 2.0f; // MINIMUM spacing of the wireframe grid lines (the small-map look). DrawWalls scales the effective spacing up with halfSize (see WALL_GRID_REF_HALF_SIZE) so big maps don't drown in grid lines.

    float elasticityPlayer = WALL_ELASTICITY_PLAYER;  // hit velocity is reflected and scaled by this (velocity = -velocity * elasticity)
    float elasticityAsteroid = WALL_ELASTICITY_ASTEROID;  // hit velocity is reflected and scaled by this (velocity = -velocity * elasticity)
    int   damage = WALL_DAMAGE;           // damage dealt to the player on wall impact

    // appearance - wireframe grid only, no translucent fill (unlike the
    // shaded-wire elements), so just an outline color.
    Color color_outline = {0, 90, 90, 255};
};

//MARK: Platform
class Platform {
public:
    // ID
    uint32_t id = 0;
    // position
    Vector3 position;
    Vector3 startingPosition; // store the initial position of the platform.
    // Vector3 velocity; // For moving platforms, future use: position += velocity * dt;
    // float speed = 0.0f; // For moving platforms, future use: speed = length(velocity);

    void updatePos(float dt) {
    // For moving platforms, future use: position = position + velocity * dt;
    }

    // size
    Vector3 size;

    void generateSize() {
        size.x = RandomFloat(PLATFORM_MIN_WIDTH, PLATFORM_MAX_WIDTH);
        size.y = RandomFloat(PLATFORM_MIN_HEIGHT, PLATFORM_MAX_HEIGHT);
        size.z = RandomFloat(PLATFORM_MIN_DEPTH, PLATFORM_MAX_DEPTH);
    }

    // appearance
    Color color_outline = {0, 255, 200, 255};
    Color color_fill = {0, 255, 200, 40}; // low alpha translucent fill for the "glowing vector glass" look

    // attributes
    bool isMoving = false; // For future use: if true, platform moves according to velocity and speed
    bool isBouncy = true; // If true, player bounces off based on elasticity factor (velocity = -velocity * elasticity)
    float elasticityPlayer = PLATFORM_ELASTICITY_PLAYER; // For bouncy platforms, 0.0 - 1.0, determines how much the player bounces (velocity = -velocity * elasticity)
    float elasticityAsteroid = PLATFORM_ELASTICITY_ASTEROID; // For bouncy platforms, 0.0 - 1.0, determines how much the asteroid bounces (velocity = -velocity * elasticity)

private:
    // static constexpr (not per-instance const) so Platform stays copy-assignable.
    // static constexpr float max_height = 0.5f;
    // static constexpr float min_height = 0.1f;
    // static constexpr float max_width = 20.0f;
    // static constexpr float min_width = 5.0f;
    // static constexpr float max_depth = max_width;
    // static constexpr float min_depth = min_width;
};


//MARK: Reticle
class Reticle {
public:
    Vector3 position; // 3D world position of the reticle, recomputed each frame (post-collision) from the anchor + aim.
    // eyeHeight is decomissoned, so Reticle::position is based on the player's center point.
    Vector3 anchor{};               // smoothed player center the reticle hangs off of (eased toward player.position)
    bool anchorInitialized = false; // seeds the anchor on the first update so it doesn't glide in from the origin
    float yaw = 0.0f;
    float pitch = 0.0f;

    // size, shape, and appearance
    float size = RETICLE_SIZE; // radius of the ring / bracket frame, units
    float rotation = 0.0f; // Rotation angle of the reticle in radians. AKA Roll.  Remains fixed, unlike pitch and yaw.  Represents the rotation around the forward axis (the direction the player is facing).
    float standoff = RETICLE_STANDOFF; // gap beyond the body surface, along the aim direction (so it clears the body)

    Color color = {255, 255, 255, 255}; // Color of the reticle

    bool isVisibleToOwner = true; // Whether the reticle is currently visible
    bool isVisibleToEnemies = true; // Whether the reticle is visible to other players (enemies)

    // Ticked once per frame AFTER collisions resolve the player's final position
    // (see main.cpp), so the reticle can never lag a frame behind a wall bounce.
    // Only the anchor (the player center) is smoothed; aim stays live so turning
    // is responsive. `smooth` is false for the local player so the crosshair
    // snaps to center (a plain smoother would lag a moving target off-center).
    void update(const Vector3& playerPosition, float playerYaw, float playerPitch,
                float playerRadius, float dt, bool smooth) {
        if (!anchorInitialized || !smooth) {
            anchor = playerPosition;             // snap (local player, or first frame)
            anchorInitialized = true;
        } else {
            float a = 1.0f - expf(-dt * RETICLE_SMOOTHING); // frame-rate-independent ease
            anchor = Vector3Lerp(anchor, playerPosition, a);
        }

        // Place the reticle in front of the (smoothed) anchor, just beyond the
        // body surface (radius + standoff) so it never renders inside the body.
        Vector3 forwardDirection = {
            cosf(playerPitch) * cosf(playerYaw),
            sinf(playerPitch),
            cosf(playerPitch) * sinf(playerYaw)
        };
        position = Vector3Add(anchor, Vector3Scale(forwardDirection, playerRadius + standoff));
    }
};




class Player {
public:
    // ID
    uint32_t id = 0; // Unique identifier for the player
    std::string name = "Player"; // Player's display name
    // Multiplayer: is this slot occupied by a connected client? Defaults true so
    // local single-player and test bots always render; the server sets it per
    // slot in the broadcast (see buildStatePacket), and the client skips drawing
    // unoccupied slots (gamespace.h draw()). Not used by the local sim.
    bool isConnected = true;

    bool isBot = false; // Whether the player is a bot or a human player.

    bool isOutOfBounds = false; // Whether the player is currently out of bounds.
    float outOfBoundsTimer = 0.0; // Timer for how long the player has been out of bounds.
    bool outOfBoundsWarned = false; // Whether the player has been warned about being out of bounds.

    bool isSpectating = false; // Whether the player is currently spectating (not actively playing).  Set in gameSpace.
    float countdownToSpectating = 5.0f; // Countdown length for the player to enter spectating mode after death.
    float SpectatingTimer = countdownToSpectating; // Timer for the player to enter spectating mode after death.

    uint32_t rocketCounter = 0; // Counter for rockets fired by this player, used to generate unique rocket IDs

    // Reticle
    Reticle reticle; // Each player has their own reticle, which is updated based on the player's position and aim direction.

    //MARK: Player Move & Pos
    Vector3 position = {0.0f, 0.0f, 0.0f}; // Center starting position.  Replaced.
    Vector3 startingPosition; // store the initial position of the player, set during game space generation.
    Vector3 velocity = {0.0f, 0.0f, 0.0f}; // Player velocity, updated by movement input and gravity

    // Height at the previous frame's platform check. Used to edge-trigger the
    // pass-through sound exactly once per traversal: fire on the frame the player
    // crosses a platform's center plane, not every frame it overlaps the box.
    // prevYValid gates the first frame (and stays false until one is recorded).
    float prevY = 0.0f;
    bool  prevYValid = false;

    // Look direction, stored as yaw/pitch rather than a raw Vector3 - this is
    // the authoritative state for both camera orientation and player facing
    // (shooting direction). A Vector3 can't cleanly accumulate frame-over-frame
    // mouse deltas or express a pitch clamp, so yaw/pitch are the source of
    // truth and Forward()/Right() below derive a Vector3 from them on demand.
    float yaw = -90.0f * DEG2RAD; // facing -Z initially
    float pitch = 0.0f;
    const float pitchLimit = 89.0f * DEG2RAD;
    const float lookSensitivity = 0.0025f;

    //MARK: Player Movement Speeds and Acceleration
    float speedWalk = PLAYER_SPEED_WALK; // units/sec
    float accelerationWalk = PLAYER_ACCELERATION_WALK; // units/sec^2, how quickly the player accelerates to their max speed when input is applied
    float speedJetpack = PLAYER_SPEED_JETPACK; // units/sec, max speed when using jetpack
    float accelerationJetpack = PLAYER_ACCELERATION_JETPACK; // units/sec^2, how quickly the player accelerates to their max jetpack speed when input is applied
    bool isUsingJetpack = false; // Whether the player is currently using the jetpack, which affects movement speed and fuel consumption
    bool earthGravityEnabled = false; // Last-applied gravity mode (mirrors this frame's input) so collision rules can read it after input is gone. Set in ApplyPlayerInput.

    // Full look-direction vector (includes pitch) - used for aiming/shooting.
    Vector3 Forward() const {
        return { cosf(pitch) * cosf(yaw), sinf(pitch), cosf(pitch) * sinf(yaw) };
    }
    Vector3 Right() const {
        Vector3 fwd = Forward();
        return Vector3Normalize(Vector3CrossProduct(fwd, {0, 1, 0}));
    }
    // Forward flattened to the horizontal plane - used for ground movement,
    // so looking up/down doesn't change walking speed or make the player fly.
    Vector3 ForwardFlat() const {
        Vector3 flat = { cosf(yaw), 0.0f, sinf(yaw) };
        return Vector3Normalize(flat);
    }

    void updateLook(Vector2 mouseDelta) {
        yaw += mouseDelta.x * lookSensitivity;
        pitch -= mouseDelta.y * lookSensitivity;
        pitch = Clamp(pitch, -pitchLimit, pitchLimit);
        // The reticle is ticked separately (updateReticle), after collisions
        // resolve the player's final position - not here in the input phase.
    }

    // Ticked each frame after collisions (main.cpp). `smooth` eases non-local
    // reticles so they can't jump; the local player passes false to keep the
    // crosshair centered and responsive.
    void updateReticle(float dt, bool smooth) {
        reticle.update(position, yaw, pitch, radius, dt, smooth);
    }

    // moveInput.x = strafe (right/left), moveInput.y = forward/back -
    // both relative to current look direction (flattened), not world axes.
    // Vertical movement (jetpack up/down) is separate, not part of moveInput.
    void updateVelocity(float dt, Vector2 moveInput, float gravity) {
        float targetSpeed = isUsingJetpack ? speedJetpack : speedWalk;
        float acceleration = isUsingJetpack ? accelerationJetpack : accelerationWalk;

        if (fuel <= FUEL_REGEN_RATE * dt) {
            targetSpeed = speedWalk; // If out of fuel, player can only walk, not jetpack.
            acceleration = accelerationWalk;
        }

        Vector3 fwd = ForwardFlat();
        Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, {0, 1, 0}));

        Vector3 desiredHorizontal{0, 0, 0};
        if (moveInput.y != 0.0f) desiredHorizontal = Vector3Add(desiredHorizontal, Vector3Scale(fwd, moveInput.y));
        if (moveInput.x != 0.0f) desiredHorizontal = Vector3Add(desiredHorizontal, Vector3Scale(right, moveInput.x));
        if (Vector3LengthSqr(desiredHorizontal) > 0.0f) {
            desiredHorizontal = Vector3Scale(Vector3Normalize(desiredHorizontal), targetSpeed);
        }

        // Accelerate horizontal velocity toward desired, same easing approach
        // as before - just split out from vertical so gravity/jetpack on the
        // Y axis isn't fighting this every frame.
        Vector3 currentHorizontal{velocity.x, 0, velocity.z};
        Vector3 horizontalChange = Vector3Subtract(desiredHorizontal, currentHorizontal);
        if (Vector3LengthSqr(horizontalChange) > 0.0f) {
            Vector3 accelVec = Vector3Scale(Vector3Normalize(horizontalChange), acceleration * dt);
            // don't overshoot past desired in one frame
            if (Vector3Length(accelVec) > Vector3Length(horizontalChange)) {
                currentHorizontal = desiredHorizontal;
            } else {
                currentHorizontal = Vector3Add(currentHorizontal, accelVec);
            }
        }
        velocity.x = currentHorizontal.x;
        velocity.z = currentHorizontal.z;

        // Vertical: jetpack thrust accelerates toward target speed rather
        // than snapping velocity.y directly - this matters because an
        // instant override would erase a wall-bounce's reflected velocity.y
        // on the very next frame if jetpack input is still held. Accelerating
        // instead means a bounce gets a moment to actually take effect.
        if (isUsingJetpack && canJetpack()) {
            float targetVerticalSpeed = speedJetpack;
            float verticalChange = targetVerticalSpeed - velocity.y;
            float maxStep = accelerationJetpack * dt;
            if (fabsf(verticalChange) > maxStep) {
                velocity.y += (verticalChange > 0 ? maxStep : -maxStep);
            } else {
                velocity.y = targetVerticalSpeed;
            }
        }
        // Gravity always applies - the jetpack has to outwork it
        // (accelerationJetpack > EARTH_GRAVITY, see constants.h). This also means
        // excess upward velocity (rocket jump, bounce) decays under thrust+gravity
        // together instead of being held while the jetpack key is down.
        velocity.y -= gravity * dt;
    }

    // boundsHalfSize is the current map's walls.halfSize (set per match by
    // GameSpace::configureMap), not the compile-time default.
    void updatePos(float dt, float boundsHalfSize) {
        position = Vector3Add(position, Vector3Scale(velocity, dt));
        // gravity is applied in updateVelocity.

        // check out of bounds.
        if (std::max({position.x, position.y, position.z}) > boundsHalfSize * GAMESPACE_OUT_OF_BOUNDS_FACTOR ||
            std::min({position.x, position.y, position.z}) < -boundsHalfSize * GAMESPACE_OUT_OF_BOUNDS_FACTOR) {
            if (!isOutOfBounds) {
                isOutOfBounds = true;
                outOfBoundsTimer = OUT_OF_BOUNDS_TIMER; // reset timer when player goes out of bounds
            } else {
                outOfBoundsTimer -= dt; // decrement timer while player is out of bounds
            }
        } else {
            isOutOfBounds = false; // player is back in bounds
            outOfBoundsWarned = false; // reset warning flag when player is back in bounds
            outOfBoundsTimer = OUT_OF_BOUNDS_TIMER; // reset timer when player is back in bounds
        }

        if (outOfBoundsTimer <= 0.0f && isOutOfBounds) {
            isAlive = false; // eliminate player if timer runs out
        }


        // Decay the damage-flash here since this runs every frame for the
        // player (gamespace.h) - mirrors how Asteroid decays its own flash.
        //MARK: Update Timers
        if (flashTimer > 0.0f) flashTimer -= dt;
        if (coolDownTime > 0.0f) coolDownTime -= dt;
        if (noFuelSfxCooldown > 0.0f) noFuelSfxCooldown -= dt;
        if (warningSfxCooldown > 0.0f) warningSfxCooldown -= dt;
        if (earthGravSfxCooldown > 0.0f) earthGravSfxCooldown -= dt;

        // Spectate delay: a dead (non-bot) player becomes a free-flying spectator
        // after a short countdown. updateFuel then refills fuel (isSpectating) so
        // updateVelocity lets them fly and the no-fuel cue stops. This runs in BOTH
        // modes via updatePositions -> updatePos: on the server for networked play
        // (authoritative; the flag/timer are synced to clients), on the client for
        // local play. Bots are excluded so a dead AI slot doesn't fly around.
        if (!isAlive && !isBot && !isSpectating) {
            SpectatingTimer -= dt;
            if (SpectatingTimer < 0.0f) isSpectating = true;
        }
    }

    // MARK: Player Size and Collision
    // The player is a SPHERE of this radius, centered on `position` (which is
    // both the collider center and the first-person camera eye). Used by the
    // renderer (shapes.h), spawn placement (gamespace.h), and all player
    // collision checks (collisions.cpp). PLAYER_SCALE tunes it (constants.h).
    float radius = PLAYER_RADIUS;

    //MARK: Player Appearance
    Color color_outline = {0, 255, 200, 255};
    Color color_fill = {0, 255, 200, 40}; // low alpha translucent fill for the "glowing vector glass" look

    // Damage-flash, same pattern as Asteroid: set to flash_duration on hit,
    // decayed in updatePos. Drives the full-screen red hurt vignette in main.cpp.
    float flashTimer = 0.0f; // seconds of damage-flash remaining

    // 1.0 right after a hit, easing to 0.0 over flash_duration.
    float flashIntensity() const {
        return flash_duration > 0.0f ? Clamp(flashTimer / flash_duration, 0.0f, 1.0f) : 0.0f;
    }

    //MARK: Health, fuel, ammo
    int health = PLAYER_STARTING_HEALTH;
    int maxHealth = PLAYER_MAX_HEALTH; // Player's maximum health, can be increased by pickups
    bool isAlive = true; // Player is alive if health > 0.
    bool deathBurstSpawned = false; // VFX guard: the one-time elimination spark burst fires once (players aren't erased).
    int damage = PLAYER_DAMAGE; // Damage dealt to other players or asteroids when colliding.
    
    int ammo = PLAYER_STARTING_AMMO;
    int maxAmmo = PLAYER_MAX_AMMO; // Player's maximum ammo, can be increased by pickups
    float fireRate = PLAYER_FIRE_RATE; // Shots per second, can be increased by pickups
    float coolDownTime = 0.0f; // Time remaining until the player can shoot again, based on fire rate
    float noFuelSfxCooldown = 0.0f; // Throttle for the "empty tank" cue while jetpack is held on empty (ticked in updatePos)
    float warningSfxCooldown = 0.0f; // Throttle for the low-resource warning alarm (ticked in updatePos)
    float earthGravSfxCooldown = 0.0f; // Guard against rapid re-triggers of the earth-gravity engage cue (ticked in updatePos)
    bool  earthGravWasEngaged = false; // Prev-frame earth-gravity state, for rising-edge detection of the engage cue
    bool  lowHealthWarned = false; // Was-low latch so the low-health message fires once per dip, not every frame (re-armed on recovery)
    bool  lowFuelWarned = false;   // Was-low latch for the low-fuel message
    bool  lowAmmoWarned = false;   // Was-low latch for the low-ammo message
    // bool canShoot = true; // Player can shoot if they have ammo, set to false when ammo reaches zero.
    float fuel = PLAYER_STARTING_FUEL; // Player starts with this much fuel, can be increased by pickups
    float maxFuel = PLAYER_MAX_FUEL; // Player's maximum fuel, can be increased by pickups
    // float fuelConsumptionRate = FUEL_CONSUMPTION_RATE; // Per sec.
    // float fuelRegenRate = FUEL_REGEN_RATE; // Per sec.  Fuel regeneration rate when not using jetpack
    bool hasFuel() const { return fuel > 0.0f; }
    // Enough fuel for the jetpack to produce thrust (updateVelocity's gate).
    // Below JETPACK_MIN_FUEL the tank reads empty even though hasFuel() may be true.
    bool canJetpack() const { return fuel > JETPACK_MIN_FUEL; }

    bool shoot() {
        bool shotFired = false;
        if (ammo > 0 && coolDownTime <= 0.0f && isAlive) {
            ammo -= 1;
            if (ammo < 0) ammo = 0; // Clamp ammo to zero
            shotFired = true;
            coolDownTime = 1.0f / fireRate; // Reset cooldown based on fire rate
        }
        return shotFired;
    }

    void updateFuel(float dt, bool isUsingJetpack) {
        if (isUsingJetpack && hasFuel()) {
            fuel -= dt * FUEL_CONSUMPTION_RATE; // Consume fuel based on time using jetpack
            if (fuel < 0.0f) fuel = 0.0f; // Clamp fuel to zero
        } else {
            fuel += dt * FUEL_REGEN_RATE; // Regenerate fuel slowly when not using jetpack
            if (fuel > PLAYER_MAX_FUEL) fuel = PLAYER_MAX_FUEL; // Clamp fuel to max
        }
        if (!isAlive) {
            fuel = 0.0f; // If player is dead, fuel is zero
        }
        if (isSpectating) {
            fuel = PLAYER_MAX_FUEL; // If player is spectating, fuel is max.  Can move freely without jetpack restrictions.
        }
    }

    //MARK: Player Damage
    void takeDamage(int damage) {
        health -= damage;
        if (health < 0) health = 0; // Clamp health to zero
        if (health == 0) {
            isAlive = false; // Player is dead if health reaches zero
        }
        flashTimer = flash_duration; // trigger the full-screen red hurt flash
        
    }

    //MARK: Player Points/Award
    int score = 0; // Player's score, can be increased by destroying asteroids or other players
    int eliminationScoreAward = PLAYER_ELIMINATION_SCORE_AWARD; // Points awarded for eliminating this player.


private:
    // Longer than the asteroid's flash so a hit reads as a sustained screen
    // flash rather than a blip. static constexpr keeps Player copy-assignable.
    static constexpr float flash_duration = 0.5f;
};

// Assign a player's outline + fill color by slot: bots get the fixed magenta,
// humans round-robin through HUMAN_PLAYER_COLORS. The fill is the same hue at
// low alpha for the translucent "glowing glass" look. Single source of truth for
// the color rule - used at spawn (gamespace.h) and re-derived each frame for
// networked slots (main.cpp), since color isn't sent over the wire.
inline void assignPlayerColor(Player& player, int index) {
    if (player.isBot) {
        player.color_outline = BOT_OUTLINE_COLOR;
        player.color_fill    = BOT_FILL_COLOR;
    } else {
        Color c = HUMAN_PLAYER_COLORS[index % HUMAN_PLAYER_COLORS.size()];
        c.a = 255; player.color_outline = c;
        c.a = 40;  player.color_fill    = c;
    }
}

//MARK: Asteroid
class Asteroid {
public:
    // ID
    uint32_t id = 0; // Unique identifier for the asteroid

    // position, velocity, speed
    Vector3 position;
    Vector3 startingPosition; // store the initial position of the asteroid, set during game space generation.
    Vector3 velocity;
    float speed;

    // Accumulated tumble angle (radians), integrated each frame in
    // GameSpace::updateAsteroidSpin. Stored (not GetTime()-based) so a bounce that
    // changes speed only changes the spin RATE going forward, never snapping orientation.
    float spinAngle = 0.0f;

    void generateVelocity() {
        // Magnitude is always positive (min_speed..max_speed); a random sign
        // per axis gives asteroids drift in any direction, not just +X/+Y/+Z.
        float speed_x = RandomFloat(ASTEROID_MIN_SPEED, ASTEROID_MAX_SPEED) * (RandomFloat(-1, 1) < 0 ? -1.0f : 1.0f);
        float speed_y = RandomFloat(ASTEROID_MIN_SPEED, ASTEROID_MAX_SPEED) * (RandomFloat(-1, 1) < 0 ? -1.0f : 1.0f);
        float speed_z = RandomFloat(ASTEROID_MIN_SPEED, ASTEROID_MAX_SPEED) * (RandomFloat(-1, 1) < 0 ? -1.0f : 1.0f);
        velocity = {speed_x, speed_y, speed_z};
        speed = Vector3Length(velocity); // keep the scalar speed in sync with the vector
    }

    void updatePos(float dt) {
        position = Vector3Add(position, Vector3Scale(velocity, dt));
        // asteroids will move in a straight line at a constant velocity.

        // Decay the damage-flash here since this runs every frame for every
        // asteroid (gamespace.h) - no separate update pass needed.
        if (flashTimer > 0.0f) flashTimer -= dt;
    }

    // shape and collision box
    float size; // radius of the asteroid
    // For rendering the asteroid, we can use a wireframe sphere or a simple 3D model.
    // For collision detection, we will use the position as the center of the sphere,
    // and size as the radius of the collision sphere.

    void generateSize() {
        size = RandomFloat(ASTEROID_MIN_SIZE, ASTEROID_MAX_SIZE);
    }

    // appearance
    Color color_outline = {255, 100, 0, 255};
    static constexpr unsigned char starting_alpha = 112; // Starting alpha value for the fill color, used to calculate fade on damage
    Color color_fill = {255, 100, 0, starting_alpha}; // low alpha translucent fill for the "glowing vector glass" look
    float flashTimer = 0.0f; // seconds of damage-flash remaining; set on hit, decayed in updatePos

    // 1.0 right after a hit, easing to 0.0 over flash_duration. Lets the
    // (non-member) draw code read flash strength without exposing the constant.
    float flashIntensity() const {
        return ASTEROID_FLASH_DURATION > 0.0f ? Clamp(flashTimer / ASTEROID_FLASH_DURATION, 0.0f, 1.0f) : 0.0f;
    }

    // attributes
    int damage = ASTEROID_DAMAGE; // Damage to player on collision
    int health = ASTEROID_STARTING_HEALTH; // Asteroid health, can be reduced by player shooting it
    bool isDestroyed = false; // Asteroid is destroyed when health reaches zero
    int scoreAward = ASTEROID_SCORE_AWARD; // Points awarded to player for destroying this asteroid
    float fuelAward = ASTEROID_FUEL_AWARD; // Fuel awarded to player for destroying this asteroid
    int healthAward = ASTEROID_HEALTH_AWARD; // Health awarded to player for destroying this asteroid
    int ammoAward = ASTEROID_AMMO_AWARD; // Ammo awarded to player for destroying this asteroid

    void takeDamage(int damage) {
        health -= damage;

        if (health < 0) health = 0; // Clamp health to zero
        if (health == 0) {
            isDestroyed = true; // Asteroid is destroyed if health reaches zero
        }

        syncFillAlpha();

        flashTimer = ASTEROID_FLASH_DURATION * (damage / 25.0f); // trigger the hot-glow damage flash
    }

    // Fill alpha tracks health: starting_alpha at full health, draining to 0
    // as health does (Fade *sets* alpha from a 0-1 ratio, so this is absolute,
    // not compounding). Called from takeDamage locally; networked clients never
    // run takeDamage, so wire.h calls this after applying a synced health.
    void syncFillAlpha() {
        float health_ratio = (float)health / (float)starting_health;
        color_fill = Fade(color_fill, health_ratio * (float)starting_alpha / 255.0f);
    }

private:

    static constexpr int starting_health = ASTEROID_STARTING_HEALTH; // Initial health of the asteroid.
};

//MARK: Rocket
class Rocket {
public:
    // ID
    uint32_t id = 0; // Unique identifier for the rocket
    // ownership. Which player fired this rocket? Stored as the player id (0 =
    // none), not a Player*, so a players-vector reallocation can't dangle it;
    // resolve with findPlayerById when the Player is needed.
    uint32_t ownerId = 0;
    // position, velocity, direction, speed
    Vector3 position;
    Vector3 prevPosition{}; // position at the start of this frame's movement; the
                            // swept collision segment is prevPosition -> position
    bool isOutOfBounds = false;
    Vector3 velocity;
    Vector3 direction; // Normalized direction vector for movement
    float speed = ROCKET_SPEED; // units/sec
    float kickback = speed * ROCKET_KICKBACK_FACTOR; // Recoil applied to player on shoot.

    bool gravityEnabled = ROCKET_GRAVITY_ENABLED; // Whether gravity affects the rocket's trajectory
    bool velocityInheritance = ROCKET_VELOCITY_INHERITANCE_ENABLED; // Whether the rocket inherits the player's velocity at the moment of firing

    void updatePos(float dt) {
        // Advance prevPosition to where we were before integrating - but skip
        // this on the first tick so the spawn-time prevPosition (set to the
        // player's eye in input.h) survives through the first collision check.
        // That makes the first swept segment span the eye -> muzzle spawn gap,
        // where a platform the player is standing on would otherwise be missed.
        if (firstStep) firstStep = false; else prevPosition = position;

        if (gravityEnabled) {
            velocity.y -= MOON_GRAVITY * dt; // Apply gravity to the rocket's velocity
        }
        // velocityInheritance is applied once at spawn (input.h), not per-frame.
        position = Vector3Add(position, Vector3Scale(velocity, dt));

        //MARK: Rocket Out-of-Bounds
        if (isOutOfBounds) {
            fadeTimer -= (dt / 1.0f);
            oobFade(1.0f - (fadeTimer / fadeLength)); // Fade from normal to OOB colors over fadeLength seconds
            if (fadeTimer <= 0.0f) isDestroyed = true;
        }
    }

    // size and collision box
    // Vector3 size = {0.2f, 0.2f, 0.8f}; // width, height, depth of the rocket's collision box (a small rectangular prism)
    float size = ROCKET_RADIUS; // Radius.
    // For rendering the rocket, we can use a wireframe rectangular prism or a simple 3D model.
    // For collision detection, we will use the position as the center of the rocket,
    // and size to define the extents of the collision box.

    // appearance
    Color color_outline = ROCKET_OUTLINE_COLOR;
    Color color_fill = ROCKET_FILL_COLOR; // low alpha translucent fill for the "glowing vector glass" look
    float fadeLength = 3.0f;
    float fadeTimer = fadeLength; // seconds of fade remaining; set on spawn, decayed in updatePos

    void oobFade (float scale) {
        color_outline.r = (int)((ROCKET_OOB_OUTLINE_COLOR.r - ROCKET_OUTLINE_COLOR.r) * scale + ROCKET_OUTLINE_COLOR.r);
        color_outline.g = (int)((ROCKET_OOB_OUTLINE_COLOR.g - ROCKET_OUTLINE_COLOR.g) * scale + ROCKET_OUTLINE_COLOR.g);
        color_outline.b = (int)((ROCKET_OOB_OUTLINE_COLOR.b - ROCKET_OUTLINE_COLOR.b) * scale + ROCKET_OUTLINE_COLOR.b);
        color_outline.a = (int)((ROCKET_OOB_OUTLINE_COLOR.a - ROCKET_OUTLINE_COLOR.a) * scale + ROCKET_OUTLINE_COLOR.a);
        color_fill.r = (int)((ROCKET_OOB_FILL_COLOR.r - ROCKET_FILL_COLOR.r) * scale + ROCKET_FILL_COLOR.r);
        color_fill.g = (int)((ROCKET_OOB_FILL_COLOR.g - ROCKET_FILL_COLOR.g) * scale + ROCKET_FILL_COLOR.g);
        color_fill.b = (int)((ROCKET_OOB_FILL_COLOR.b - ROCKET_FILL_COLOR.b) * scale + ROCKET_FILL_COLOR.b);
        color_fill.a = (int)((ROCKET_OOB_FILL_COLOR.a - ROCKET_FILL_COLOR.a) * scale + ROCKET_FILL_COLOR.a);
    }

    // attributes
    // damage and explosion radius are properties of the resulting Explosion.
    bool isDestroyed = false; // Rocket is destroyed on collision or when it goes out of bounds

private:
    bool firstStep = true; // see updatePos: preserves the spawn eye->muzzle segment
};

//MARK: Explosion
class Explosion {
public:
    // ID
    uint32_t id = 0; // Unique identifier for the explosion
    // ownership. Which player fired the rocket that caused this explosion?
    // Player id (0 = none), not a Player* - see Rocket::ownerId.
    uint32_t ownerId = 0;
    // position and size
    Vector3 position;
    float radius = 0.0f; // Current radius of the explosion effect
    float maxRadius = EXPLOSION_MAX_RADIUS; // Maximum radius of the explosion effect
    float expansionRate = EXPLOSION_EXPANSION_RATE; // How quickly the explosion expands, in units/sec
    bool isActive = true; // Whether the explosion effect is still active (expanding or at max radius)

    // splash damage - set from the originating Rocket's damage/damageRadius
    // at spawn time, since the rocket itself is destroyed/erased
    // before the explosion finishes its visual lifetime.
    int damage = EXPLOSION_DAMAGE; // Max damage dealt to anything within damageRadius of position
    float damageRadius = EXPLOSION_DAMAGE_RADIUS; // Radius of the splash damage area (separate from visual maxRadius)
    bool hasAppliedDamage = false; // Splash damage applies once, at the moment of explosion creation - not every frame
    float pushbackFactor = EXPLOSION_PUSHBACK_FACTOR; // Factor to scale the pushback force applied to objects within the explosion radius

    // appearance
    Color color_outline = {255, 150, 0, 255};
    Color color_fill = {255, 150, 0, 40}; // low alpha translucent fill for the "glowing vector glass" look

    void update(float dt) {
        if (isActive) {
            radius += expansionRate * dt;
            color_fill.a = (unsigned char)(40 * (1 - radius / maxRadius)); // Fade out fill color as explosion expands
            if (radius >= maxRadius) {
                radius = maxRadius;
                isActive = false; // Explosion has reached its maximum size
            }
        }
    }



private:
};

//MARK: Spark (VFX particle)
// A lightweight visual-only particle (no collision). Spawned as a one-time
// asteroid-destruction / player-elimination burst, drifts, and fades over its
// lifetime. Mirrors Explosion's "tick + isActive" lifecycle so GameSpace can
// own/update/erase it through the same pipeline.
class Spark {
public:
    Vector3 position{0, 0, 0};
    Vector3 velocity{0, 0, 0};
    float age = 0.0f;
    float lifetime = 0.5f;
    Color color = {255, 230, 150, 255};
    bool isActive = true;

    void update(float dt) {
        velocity.y -= SPARK_GRAVITY * dt;        // light gravity so bursts arc
        float damp = 1.0f - SPARK_DRAG * dt;     // air drag
        if (damp < 0.0f) damp = 0.0f;
        velocity = Vector3Scale(velocity, damp);
        position = Vector3Add(position, Vector3Scale(velocity, dt));
        age += dt;
        if (age >= lifetime) isActive = false;
    }

    // 1.0 at birth -> 0.0 at death, for alpha fade.
    float fade() const { return lifetime > 0.0f ? Clamp(1.0f - age / lifetime, 0.0f, 1.0f) : 0.0f; }
};

//MARK: Spark emitter (asteroid / player elimination bursts)
// (A cone emitter for the jetpack exhaust plume was deprecated and archived in
// docs/exhaust-plume-archive.md.)

// Emit `count` sparks uniformly in all directions from `origin` - a one-time
// spherical puff (e.g. an asteroid breaking apart, or a player elimination).
inline void SpawnSparkBurst(std::vector<Spark>& out, Vector3 origin,
                            float speedMin, float speedMax, int count, Color color,
                            float lifeMin, float lifeMax) {
    for (int i = 0; i < count; i++) {
        float z = RandomFloat(-1.0f, 1.0f);          // uniform point on the unit sphere
        float t = RandomFloat(0.0f, 2.0f * PI);
        float r = sqrtf(1.0f - z * z);
        Vector3 d = {r * cosf(t), z, r * sinf(t)};
        Spark s;
        s.position = origin;
        s.velocity = Vector3Scale(d, RandomFloat(speedMin, speedMax));
        s.lifetime = RandomFloat(lifeMin, lifeMax);
        s.color = color;
        out.push_back(s);
    }
}