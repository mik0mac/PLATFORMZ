// C++ header file with the classes and functions related to the game logic.  Using raylib 3d.

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>
#include <random>

const float GRAVITY = 1.62f; // moon gravity, m/s^2 (assuming 1 unit = 1 meter)

//MARK: GameSpace
class GameSpace {
public:
    float halfSize = 40.0; // 1.0 equates to a meter. Defines the half-size of the cubic game space, so total size is 80x80x80
    Color backgroundColor = {0, 0, 0, 255}; // Black background for the game space
    Color gridColor = {0, 90, 90, 255}; // Color for the ground grid lines

    float wall_elasticity = 1.0f; // When player hits the walls of the game space, velocity is reflected and scaled by this factor (velocity = -velocity * wall_elasticity)
    int wall_damage = 10; // When player hits the walls of the game space, they take this much damage

    void generate() {
        platforms.clear();
        for (int i = 0; i < num_of_platforms; ++i) {
            Platform platform;
            platform.size = {GetRandomValue(1, 5), 0.5f, GetRandomValue(1, 5)}; // Random width and depth, thin height for a platform
            platform.position = {GetRandomValue(-halfSize, halfSize), GetRandomValue(-halfSize, halfSize), GetRandomValue(-halfSize, halfSize)};
            platform.startingPosition = platform.position; // Store the initial position of the platform
            platforms.push_back(platform);
        }
        asteroids.clear();
        for (int i = 0; i < num_of_asteroids; ++i) {
            Asteroid asteroid;
            asteroid.size = GetRandomValue(0.5f, 3.0f);
            asteroid.position = {GetRandomValue(-halfSize, halfSize), GetRandomValue(-halfSize, halfSize), GetRandomValue(-halfSize, halfSize)};
            asteroid.startingPosition = asteroid.position; // Store the initial position of the asteroid
            asteroid.velocity = {GetRandomValue(-2, 2), GetRandomValue(-2, 2), GetRandomValue(-2, 2)}; // Random velocity in each direction
            asteroids.push_back(asteroid);
        }
        players.clear();
        for (int i = 0; i < number_of_players; ++i) {
            Player player;
            // chose a random platform and place the player on top of it as the starting position
            if (!platforms.empty()) {
                int platformIndex = GetRandomValue(0, platforms.size() - 1);
                Platform& startPlatform = platforms[platformIndex];
                player.position = Vector3Add(startPlatform.position, Vector3{0, startPlatform.size.y / 2 + player.size.y / 2, 0}); // Position player on top of the platform
            } else {
                player.position = {0.0f, 0.0f, 0.0f}; // If no platforms, start at the center of the game space
            }
            player.startingPosition = player.position; // Store the initial position of the player

            // player direction starts facing towards the center of the game space.
            player.direction = Vector3Normalize(Vector3Subtract({0.0f, 0.0f, 0.0f}, player.position));

            // player velocity starts at zero.  Default speed is set in the Player class.
            // player health, fuel, and ammo start at their default values defined in the Player class.
            
            players.push_back(player);
        }
    
   

private:
    int num_of_platforms = 8;
    std::vector<Platform> platforms;
    int num_of_asteroids = 5;
    std::vector<Asteroid> asteroids;
    int number_of_players = 1; // For future use: if we want to add multiplayer support, we can increase this and add a vector of Player objects.
    std::vector<Player> players;

}

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
    float speed = 5.0; // units/sec

    void updatePos(float dt) {
        // Apply gravity
        velocity.y -= GRAVITY * dt;

        // Update position based on velocity
        position = Vector3Add(position, Vector3Scale(velocity, dt));

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
    bool isAlive() const { return health > 0; }
    int ammo = 10;
    bool canShoot() const { return ammo > 0; }
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
}

//MARK: Asteroid
class Asteroid {
public:
    // position, velocity, size
    Vector3 position;
    Vector3 startingPosition; // store the initial position of the asteroid, set during game space generation.
    Vector3 velocity;
    float size; // radius of the asteroid

    // appearance
    Color color_outline = {255, 100, 0, 255};
    Color color_fill = {255, 100, 0, 40}; // low alpha translucent fill for the "glowing vector glass" look

    // attributes
    int damage = 20; // Damage to player on collision
    int health = 50; // Asteroid health, can be reduced by player shooting it
    bool isDestroyed() const { return health <= 0; }
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
}

//MARK: Rocket
class Rocket {
public:
    // position, velocity, direction, speed
    Vector3 position;
    Vector3 velocity;
    Vector3 direction; // Normalized direction vector for movement
    float speed = 10.0f; // units/sec

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
}