#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>

#include <algorithm>

#include "elements.h"
#include "shapes.h"
#include "random.h"


//MARK: GameSpace
class GameSpace {
public:
    float halfSize = 40.0; // 1.0 equates to a meter. Defines the half-size of the cubic game space, so total size is 80x80x80
    Color backgroundColor = {0, 0, 0, 255}; // Black background for the game space
    Color gridColor = {0, 90, 90, 255}; // Color for the ground grid lines

    float wall_elasticity = 1.0f; // When player or asteroid hits the walls of the game space, velocity is reflected and scaled by this factor (velocity = -velocity * wall_elasticity)
    int wall_damage = 10; // When player hits the walls of the game space, they take this much damage

    void generate() {
        platforms.clear();
        for (int i = 0; i < num_of_platforms; ++i) {
            Platform platform;
            platform.generateSize(); // Random width and depth, thin height for a platform
            platform.position = {RandomFloat(-halfSize, halfSize), RandomFloat(-halfSize, halfSize), RandomFloat(-halfSize, halfSize)};
            platform.startingPosition = platform.position; // Store the initial position of the platform
            platforms.push_back(platform);
        }
        asteroids.clear();
        for (int i = 0; i < num_of_asteroids; ++i) {
            Asteroid asteroid;
            asteroid.generateSize(); // Random size for the asteroid
            asteroid.position = {RandomFloat(-halfSize, halfSize), RandomFloat(-halfSize, halfSize), RandomFloat(-halfSize, halfSize)};
            asteroid.startingPosition = asteroid.position; // Store the initial position of the asteroid
            asteroid.generateVelocity(); // Random velocity in each direction
            asteroids.push_back(asteroid);
        }
        players.clear();
        for (int i = 0; i < number_of_players; ++i) {
            Player player;
            // chose a random platform and place the player on top of it as the starting position
            if (!platforms.empty()) {
                int platformIndex = static_cast<int>(RandomFloat(0, platforms.size() - 1));
                Platform& startPlatform = platforms[platformIndex];
                player.position = Vector3Add(startPlatform.position, Vector3{0, startPlatform.size.y / 2 + player.size.y / 2, 0}); // Position player on top of the platform
            } else {
                player.position = {0.0f, 0.0f, 0.0f}; // If no platforms, start at the center of the game space
            }
            player.startingPosition = player.position; // Store the initial position of the player

            // player direction starts facing towards the center of the game space.
            Vector3 toCenter = Vector3Subtract({0, 0, 0}, player.position);
            player.yaw = atan2f(toCenter.z, toCenter.x); // Yaw is the angle in the XZ plane, so use atan2 with z and x.
            player.pitch = 0.0f; // Start looking horizontally, no pitch.

            // player velocity starts at zero.  Default speed is set in the Player class.
            // player health, fuel, and ammo start at their default values defined in the Player class.
            
            players.push_back(player);
        }
        rockets.clear(); // Rockets will be generated when the player shoots.
        explosions.clear(); // Explosions will be generated when rockets detonate.
    };

    void updatePositions(float dt) {
        // Update platforms (for moving platforms, future use)
        for (Platform& platform : platforms) {
            if (platform.isMoving) {
                platform.updatePos(dt);
            }
        }

        // Update players
        for (Player& player : players) {
            player.updatePos(dt);
        }


        // Update asteroids
        for (Asteroid& asteroid : asteroids) {
            asteroid.updatePos(dt);
        }

        // Update rockets
        for (Rocket& rocket : rockets) {
            rocket.updatePos(dt);
        }

        // Update explosions (expand + fade). Finished ones (isActive == false)
        // are removed in updateActiveObjects().
        for (Explosion& explosion : explosions) {
            explosion.update(dt);
        }

    }

    void updateActiveObjects() {
        // Remove destroyed asteroids
        asteroids.erase(std::remove_if(asteroids.begin(), asteroids.end(),
            [](const Asteroid& asteroid) { return asteroid.isDestroyed; }), asteroids.end());

        // Remove destroyed rockets
        rockets.erase(std::remove_if(rockets.begin(), rockets.end(),
            [](const Rocket& rocket) { return rocket.isDestroyed; }), rockets.end());

        // Remove finished explosions
        explosions.erase(std::remove_if(explosions.begin(), explosions.end(),
            [](const Explosion& explosion) { return !explosion.isActive; }), explosions.end());
    }

    //MARK: Draw
    // Must be called between BeginMode3D(camera)/EndMode3D() in main.cpp -
    // GameSpace itself doesn't own or know about the camera, that's an
    // orchestration concern that belongs at the main.cpp level.
    void draw() {
        DrawGridRoom(halfSize, 2.0f, gridColor);

        for (Platform& platform : platforms) {
            DrawPlatform(platform);
        }
        // NOTE: not drawing players here - in first-person, the locally
        // controlled player's own collision box would render directly
        // around the camera (you'd see your own "body" as a box at your
        // feet). Revisit this once there's a reason to render players -
        // e.g. a third-person view, or other players in a multiplayer game.
        for (Asteroid& asteroid : asteroids) {
            DrawAsteroid(asteroid);
        }
        for (Rocket& rocket : rockets) {
            DrawRocket(rocket);
        }
        for (Explosion& explosion : explosions) {
            DrawExplosion(explosion);
        }
    }

    //MARK: Accessors
    // Collision detection/response (collisions.h) and other external systems
    // need direct access to these vectors. Returned by reference so callers
    // can both iterate and mutate (e.g. taking damage, marking destroyed,
    // pushing new rockets/explosions) without GameSpace exposing raw members.
    std::vector<Platform>& getPlatforms() { return platforms; }
    std::vector<Asteroid>& getAsteroids() { return asteroids; }
    std::vector<Player>& getPlayers() { return players; }
    std::vector<Rocket>& getRockets() { return rockets; }
    std::vector<Explosion>& getExplosions() { return explosions; }

private:
    int num_of_platforms = 8;
    std::vector<Platform> platforms;
    int num_of_asteroids = 5;
    std::vector<Asteroid> asteroids;
    int number_of_players = 1; // For future use: if we want to add multiplayer support,
    // we can increase this and add a vector of Player objects.
    std::vector<Player> players;
    int number_of_rockets = 0; // Rockets will be generated when the player shoots.
    std::vector<Rocket> rockets;
    int number_of_explosions = 0; // Explosions will be generated when rockets detonate.
    std::vector<Explosion> explosions;
};
