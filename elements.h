// C++ header file with the classes and functions related to the game logic.  Using raylib 3d.

#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>
#include <random>

const float GRAVITY = 1.62f; // moon gravity, m/s^2 (assuming 1 unit = 1 meter)


//MARK: Platform
class Platform {
public:
    // position and size
    Vector3 size;
    Vector3 position;
    Vector3 startingPosition; // store the initial position of the platform.
    // Vector3 velocity; // For moving platforms, future use: position += velocity * dt;
    // float speed = 0.0f; // For moving platforms, future use: speed = length(velocity);

    // appearance
    Color color_outline = {0, 255, 200, 255};
    Color color_fill = {0, 255, 200, 40}; // low alpha translucent fill for the "glowing vector glass" look

    // attributes
    bool isMoving = false; // For future use: if true, platform moves according to velocity and speed
    bool isBouncy = false; // If true, player bounces off based on elasticity factor (velocity = -velocity * elasticity)
    float elasticity = 0.5f; // For bouncy platforms, 0.0 - 1.0, determines how much the player bounces (velocity = -velocity * elasticity)

    private:
};

//MARK: Player
class Player {
public:
    // movement and facing
    Vector3 position = {0.0f, 0.0f, 0.0f}; // Center starting position.  Replaced.
    Vector3 startingPosition; // store the initial position of the player, set during game space generation.
    Vector3 velocity = {0.0f, 0.0f, 0.0f}; // Player velocity, updated by movement input and gravity
    Vector3 direction = {0.0f, 0.0f, 1.0f}; // Normalized direction vector for movement and facing
    float speedWalk = 5.0f; // units/sec
    float accelerationWalk = 5.0f; // units/sec^2, how quickly the player accelerates to their max speed when input is applied
    float speedJetpack = 8.0f; // units/sec, max speed when using jetpack
    float accelerationJetpack = 10.0f; // units/sec^2, how quickly the player accelerates to their max jetpack speed when input is applied
    bool isUsingJetpack = false; // Whether the player is currently using the jetpack, which affects movement speed and fuel consumption

    void updateDirection(Vector3 inputDirection) {
        // Update the player's facing direction based on input.  This is separate from movement to allow for strafing.
        if (inputDirection.x != 0 || inputDirection.y != 0 || inputDirection.z != 0) {
            direction = Vector3Normalize(inputDirection);
        }
    }

    void updateVelocity(float dt, Vector3 inputDirection) {
        // Update the player's velocity based on input and whether they are using the jetpack.
        float targetSpeed = isUsingJetpack ? speedJetpack : speedWalk;
        float acceleration = isUsingJetpack ? accelerationJetpack : accelerationWalk;

        Vector3 desiredVelocity = Vector3Scale(Vector3Normalize(inputDirection), targetSpeed);
        Vector3 velocityChange = Vector3Subtract(desiredVelocity, velocity);
        Vector3 accelerationVector = Vector3Scale(Vector3Normalize(velocityChange), acceleration);

        // Update velocity with acceleration, but don't exceed target speed
        velocity = Vector3Add(velocity, Vector3Scale(accelerationVector, dt));
        if (Vector3Length(velocity) > targetSpeed) {
            velocity = Vector3Scale(Vector3Normalize(velocity), targetSpeed);
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
        // Create a new Rocket object and set its initial position, velocity, and direction based on the player's current facing direction.
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
    float speed = 5.0f; // units/sec

    void updatePos(float dt) {
        position = Vector3Add(position, Vector3Scale(velocity, dt));
        // asteroids will move in a straight line at a constant velocity.
    }

    // shape and collision box
    float size; // radius of the asteroid
    // For rendering the asteroid, we can use a wireframe sphere or a simple 3D model.
    // For collision detection, we will use the position as the center of the sphere,
    // and size as the radius of the collision sphere.

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
};

//MARK: Rocket
class Rocket {
public:
    // position, velocity, direction, speed
    Vector3 position;
    Vector3 velocity;
    Vector3 direction; // Normalized direction vector for movement
    float speed = 10.0f; // units/sec

    void updatePos(float dt) {
        velocity.y -= GRAVITY * dt; // Apply gravity to the rocket's velocity
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