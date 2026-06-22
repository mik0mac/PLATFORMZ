// C++ header file with the classes and functions related to the game logic.  Using raylib 3d.

#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>

#include "random.h"

const float GRAVITY = 1.62f; // moon gravity, m/s^2 (assuming 1 unit = 1 meter)


//MARK: Platform
class Platform {
public:
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
        size.x = RandomFloat(min_width, max_width);
        size.y = RandomFloat(min_height, max_height);
        size.z = RandomFloat(min_depth, max_depth);
    }

    // appearance
    Color color_outline = {0, 255, 200, 255};
    Color color_fill = {0, 255, 200, 40}; // low alpha translucent fill for the "glowing vector glass" look

    // attributes
    bool isMoving = false; // For future use: if true, platform moves according to velocity and speed
    bool isBouncy = false; // If true, player bounces off based on elasticity factor (velocity = -velocity * elasticity)
    float elasticity = 0.5f; // For bouncy platforms, 0.0 - 1.0, determines how much the player bounces (velocity = -velocity * elasticity)

private:
    // static constexpr (not per-instance const) so Platform stays copy-assignable.
    static constexpr float max_height = 0.5f;
    static constexpr float min_height = 0.1f;
    static constexpr float max_width = 20.0f;
    static constexpr float min_width = 5.0f;
    static constexpr float max_depth = max_width;
    static constexpr float min_depth = min_width;
};

//MARK: Player
class Player {
public:
    // movement and facing
    Vector3 position = {0.0f, 0.0f, 0.0f}; // Center starting position.  Replaced.
    Vector3 startingPosition; // store the initial position of the player, set during game space generation.
    Vector3 velocity = {0.0f, 0.0f, 0.0f}; // Player velocity, updated by movement input and gravity

    // Look direction, stored as yaw/pitch rather than a raw Vector3 - this is
    // the authoritative state for both camera orientation and player facing
    // (shooting direction). A Vector3 can't cleanly accumulate frame-over-frame
    // mouse deltas or express a pitch clamp, so yaw/pitch are the source of
    // truth and Forward()/Right() below derive a Vector3 from them on demand.
    float yaw = -90.0f * DEG2RAD; // facing -Z initially, matches FlyCam's default
    float pitch = 0.0f;
    const float pitchLimit = 89.0f * DEG2RAD;
    const float lookSensitivity = 0.0025f;

    float speedWalk = 5.0f; // units/sec
    float accelerationWalk = 5.0f; // units/sec^2, how quickly the player accelerates to their max speed when input is applied
    float speedJetpack = 8.0f; // units/sec, max speed when using jetpack
    float accelerationJetpack = 10.0f; // units/sec^2, how quickly the player accelerates to their max jetpack speed when input is applied
    bool isUsingJetpack = false; // Whether the player is currently using the jetpack, which affects movement speed and fuel consumption

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
    }

    // moveInput.x = strafe (right/left), moveInput.y = forward/back -
    // both relative to current look direction (flattened), not world axes.
    // Vertical movement (jetpack up/down) is separate, not part of moveInput.
    void updateVelocity(float dt, Vector2 moveInput, bool jetpackUp, bool jetpackDown) {
        float targetSpeed = isUsingJetpack ? speedJetpack : speedWalk;
        float acceleration = isUsingJetpack ? accelerationJetpack : accelerationWalk;

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
        if (isUsingJetpack && (jetpackUp || jetpackDown)) {
            float targetVerticalSpeed = jetpackUp ? speedJetpack : -speedJetpack;
            float verticalChange = targetVerticalSpeed - velocity.y;
            float maxStep = accelerationJetpack * dt;
            if (fabsf(verticalChange) > maxStep) {
                velocity.y += (verticalChange > 0 ? maxStep : -maxStep);
            } else {
                velocity.y = targetVerticalSpeed;
            }
        }
    }

    void updatePos(float dt) {
        // Apply gravity
        velocity.y -= GRAVITY * dt;
        // Update position based on velocity
        position = Vector3Add(position, Vector3Scale(velocity, dt));
    }


    // shape, size and collision box
    Vector3 size = {1.0f, 2.0f, 1.0f}; // width, height, depth of the player's collision box (a vertical rectangular prism)
    // For rendering the player, we can use a wireframe rectangular prism or a simple 3D model.
    // For collision detection, we will use the position as the center of the bottom face of the prism,
    // and size to define the extents of the collision box.

    // appearance
    Color color_outline = {0, 255, 200, 255};
    Color color_fill = {0, 255, 200, 40}; // low alpha translucent fill for the "glowing vector glass" look

    // health, fuel, ammo
    int health = 100;
    bool isAlive = true; // Player is alive if health > 0.
    int ammo = 10;
    bool canShoot = true; // Player can shoot if they have ammo, set to false when ammo reaches zero.
    float fuel = 100.0f;
    float fuelRegenRate = 0.25f; // Fuel regeneration rate when not using jetpack
    bool hasFuel() const { return fuel > 0.0f; }

    void shoot() {
        // game loop will check if player can shoot.
        ammo -= 1;
        if (ammo < 0) ammo = 0; // Clamp ammo to zero
        if (ammo == 0) {
            canShoot = false; // Player cannot shoot if ammo reaches zero
        }
        // Create a new Rocket object and set its initial position, velocity, and direction based on Forward().
        // This will be handled in the main game loop where we manage the list of rockets. 
    }

    void updateFuel(float dt, bool isUsingJetpack) {
        if (isUsingJetpack && hasFuel()) {
            fuel -= dt; // Consume fuel based on time using jetpack
            if (fuel < 0.0f) fuel = 0.0f; // Clamp fuel to zero
        } else {
            fuel += dt * fuelRegenRate; // Regenerate fuel slowly when not using jetpack
            if (fuel > 100.0f) fuel = 100.0f; // Clamp fuel to max
        }
    }

    void takeDamage(int damage) {
        health -= damage;
        if (health < 0) health = 0; // Clamp health to zero
        if (health == 0) {
            isAlive = false; // Player is dead if health reaches zero
        }
    }

    
private:
};

//MARK: Asteroid
class Asteroid {
public:
    // position, velocity, speed
    Vector3 position;
    Vector3 startingPosition; // store the initial position of the asteroid, set during game space generation.
    Vector3 velocity;
    float speed;

    void generateVelocity() {
        // Magnitude is always positive (min_speed..max_speed); a random sign
        // per axis gives asteroids drift in any direction, not just +X/+Y/+Z.
        float speed_x = RandomFloat(min_speed, max_speed) * (RandomFloat(-1, 1) < 0 ? -1.0f : 1.0f);
        float speed_y = RandomFloat(min_speed, max_speed) * (RandomFloat(-1, 1) < 0 ? -1.0f : 1.0f);
        float speed_z = RandomFloat(min_speed, max_speed) * (RandomFloat(-1, 1) < 0 ? -1.0f : 1.0f);
        velocity = {speed_x, speed_y, speed_z};
        speed = Vector3Length(velocity); // keep the scalar speed in sync with the vector
    }

    void updatePos(float dt) {
        position = Vector3Add(position, Vector3Scale(velocity, dt));
        // asteroids will move in a straight line at a constant velocity.
    }

    // shape and collision box
    float size; // radius of the asteroid
    // For rendering the asteroid, we can use a wireframe sphere or a simple 3D model.
    // For collision detection, we will use the position as the center of the sphere,
    // and size as the radius of the collision sphere.

    void generateSize() {
        size = RandomFloat(min_size, max_size);
    }

    // appearance
    Color color_outline = {255, 100, 0, 255};
    Color color_fill = {255, 100, 0, 40}; // low alpha translucent fill for the "glowing vector glass" look

    // attributes
    int damage = 20; // Damage to player on collision
    int health = 50; // Asteroid health, can be reduced by player shooting it
    bool isDestroyed = false; // Asteroid is destroyed when health reaches zero
    int scoreValue = 100; // Points awarded to player for destroying this asteroid
    int fuelAward = 20; // Fuel awarded to player for destroying this asteroid
    int healthAward = 10; // Health awarded to player for destroying this asteroid

    void takeDamage(int damage) {
        health -= damage;
        if (health < 0) health = 0; // Clamp health to zero
        if (health == 0) {
            isDestroyed = true; // Asteroid is destroyed if health reaches zero
        }
    }

private:
    // static constexpr (not per-instance const) so Asteroid stays
    // copy-assignable - std::remove_if/erase in GameSpace needs that.
    static constexpr float min_size = 2.0f; // Minimum radius of the asteroid
    static constexpr float max_size = 8.0f; // Maximum radius of the asteroid
    static constexpr float min_speed = 8.0f; // Minimum speed of the asteroid
    static constexpr float max_speed = 12.0f; // Maximum speed of the asteroid
};

//MARK: Rocket
class Rocket {
public:
    // position, velocity, direction, speed
    Vector3 position;
    Vector3 velocity;
    Vector3 direction; // Normalized direction vector for movement
    float speed = 40.0f; // units/sec

    void updatePos(float dt) {
        velocity.y * dt; // Apply gravity to the rocket's velocity
        position = Vector3Add(position, Vector3Scale(velocity, dt));
    }

    // size and collision box
    Vector3 size = {0.2f, 0.2f, 0.8f}; // width, height, depth of the rocket's collision box (a small rectangular prism)
    // For rendering the rocket, we can use a wireframe rectangular prism or a simple 3D model.
    // For collision detection, we will use the position as the center of the rocket,
    // and size to define the extents of the collision box.

    // appearance
    Color color_outline = {255, 0, 0, 255};
    Color color_fill = {255, 0, 0, 40}; // low alpha translucent fill for the "glowing vector glass" look

    // attributes
    int damage = 25; // Damage to player or asteroid on collision
    float damageRadius = 5.0f; // Radius of the explosion damage area
    // will need a function to check for collisions with players and asteroids within this radius
    // and apply damage to accordingly, like damage * (1 - distance_to_target / damageRadius).
    bool isDestroyed = false; // Rocket is destroyed on collision or when it goes out of bounds
    bool isExploding = false; // Rocket is in the process of exploding, can be used for visual effects

private:
};

//MARK: Explosion
class Explosion {
public:
    // position and size
    Vector3 position;
    float radius = 0.0f; // Current radius of the explosion effect
    float maxRadius = 5.0f; // Maximum radius of the explosion effect
    float expansionRate = 20.0f; // How quickly the explosion expands, in units/sec
    bool isActive = true; // Whether the explosion effect is still active (expanding or at max radius)

    // splash damage - set from the originating Rocket's damage/damageRadius
    // at spawn time, since the rocket itself is typically destroyed/erased
    // before the explosion finishes its visual lifetime.
    int damage = 25; // Max damage dealt to anything within damageRadius of position
    float damageRadius = 5.0f; // Radius of the splash damage area (separate from visual maxRadius)
    bool hasAppliedDamage = false; // Splash damage applies once, at the moment of explosion creation - not every frame

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