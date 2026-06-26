#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>

#include <algorithm>
#include <limits>

#include "elements.h"
#include "shapes.h"
#include "random.h"
#include "constants.h"


//MARK: GameSpace
class GameSpace {
public:
    float boundaryBufferPlatforms = 0.8f; // Buffer distance to keep platforms from spawning right on the boundary.  As a factor of walls.halfSize.
    float boundaryBufferAsteroids = 0.9f; // Buffer distance to keep objects from spawning right on the boundary.  As a factor of walls.halfSize.
    Color backgroundColor = {0, 0, 0, 255}; // Black background for the game space
    
    uint32_t nextID = NON_PLAYER_ID_BASE; // Platforms and Asteroids (0x00000100) 256, 257, 258, ... 
    uint32_t nextPlayerID = PLAYER_ID_BASE; // (0x00000001) 1, 2, 3, ... 255.
    // Rockets' IDs generated in input.h


    void generate() {
        platforms.clear();
        std::vector<Vector3> placed; // already-placed platform centers, fed to best-candidate sampling
        placed.reserve(num_of_platforms);
        float platformBuffer = walls.halfSize * boundaryBufferPlatforms;
        for (int i = 0; i < num_of_platforms; ++i) {
            Platform platform;
            //MARK: Platform ID
            platform.id = nextID++;
            
            // size and position are generated randomly:
            platform.generateSize(); // Random width and depth, thin height for a platform
            platform.position = bestCandidatePosition(placed, platformBuffer); // spread platforms, avoid clustering
            platform.startingPosition = platform.position; // Store the initial position of the platform
            placed.push_back(platform.position);
            platforms.push_back(platform);
        }
        asteroids.clear();
        for (int i = 0; i < num_of_asteroids; ++i) {
            Asteroid asteroid;
            //MARK: Asteroid ID
            asteroid.id = nextID++;
            
            // size, position, and velocity are generated randomly:
            asteroid.generateSize(); // Random size for the asteroid
            float buffer = walls.halfSize * boundaryBufferAsteroids;
            asteroid.position = {RandomFloat(-buffer, buffer), RandomFloat(-buffer, buffer), RandomFloat(-buffer, buffer)};
            asteroid.startingPosition = asteroid.position; // Store the initial position of the asteroid
            asteroid.generateVelocity(); // Random velocity in each direction
            asteroids.push_back(asteroid);
        }
        players.clear();
        for (int i = 0; i < number_of_players; ++i) {
            Player player;
            //MARK: Player ID
            player.id = nextPlayerID++;
            // chose a random platform and place the player on top of it as the starting position
            if (!platforms.empty()) {
                int platformIndex = static_cast<int>(RandomFloat(0, platforms.size() - 1));
                Platform& startPlatform = platforms[platformIndex];
                player.position = Vector3Add(startPlatform.position, Vector3{0, startPlatform.size.y / 2 + player.radius, 0}); // Position player on top of the platform
            } else {
                player.position = {0.0f, 0.0f, 0.0f}; // If no platforms, start at the center of the game space
            }
            player.startingPosition = player.position; // Store the initial position of the player

            // Non-local players (index 1+, the test bots) get a distinct magenta
            // palette so they read clearly against the cyan local-player look.
            if (i > 0) {
                player.color_outline = {255, 0, 200, 255};
                player.color_fill = {255, 0, 200, 40};
            }

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
        sparks.clear(); // VFX particles, spawned by jetpack exhaust / asteroid bursts.
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

        // Update VFX sparks (drift + fade). Dead ones removed in updateActiveObjects().
        for (Spark& spark : sparks) {
            spark.update(dt);
        }
    }

    void updateActiveObjects() {
        // Spawn a one-time spherical spark burst for each asteroid about to be
        // removed. Done here so it fires regardless of what destroyed it (rocket
        // splash, player impact, ...) - a single centralized hook.
        Color burst = {255, 180, 60, 255}; // hot orange break-up (shared by both bursts)
        for (const Asteroid& asteroid : asteroids) {
            if (asteroid.isDestroyed) {
                SpawnSparkBurst(sparks, asteroid.position,
                                ASTEROID_BURST_SPEED_MIN, ASTEROID_BURST_SPEED_MAX,
                                ASTEROID_BURST_COUNT, burst,
                                SPARK_BURST_LIFETIME_MIN, SPARK_BURST_LIFETIME_MAX);
            }
        }

        // One-time spark burst when a player is eliminated. Players aren't erased
        // (the local player drives the death screen), so a per-player flag keeps
        // the burst from re-firing every frame.
        for (Player& player : players) {
            if (!player.isAlive && !player.deathBurstSpawned) {
                SpawnSparkBurst(sparks, player.position,
                                ASTEROID_BURST_SPEED_MIN, ASTEROID_BURST_SPEED_MAX,
                                ASTEROID_BURST_COUNT, burst,
                                SPARK_BURST_LIFETIME_MIN, SPARK_BURST_LIFETIME_MAX);
                player.deathBurstSpawned = true;
            }
        }

        // Remove destroyed asteroids
        asteroids.erase(std::remove_if(asteroids.begin(), asteroids.end(),
            [](const Asteroid& asteroid) { return asteroid.isDestroyed; }), asteroids.end());

        // Remove destroyed rockets
        rockets.erase(std::remove_if(rockets.begin(), rockets.end(),
            [](const Rocket& rocket) { return rocket.isDestroyed; }), rockets.end());

        // Remove finished explosions
        explosions.erase(std::remove_if(explosions.begin(), explosions.end(),
            [](const Explosion& explosion) { return !explosion.isActive; }), explosions.end());

        // Remove faded sparks
        sparks.erase(std::remove_if(sparks.begin(), sparks.end(),
            [](const Spark& spark) { return !spark.isActive; }), sparks.end());
    }

    //MARK: Draw
    // Must be called between BeginMode3D(camera)/EndMode3D() in main.cpp -
    // GameSpace itself doesn't own or know about the camera, that's an
    // orchestration concern that belongs at the main.cpp level.
    void draw(int localPlayerIndex = -1) {
        DrawWalls(walls);

        for (Platform& platform : platforms) {
            DrawPlatform(platform);
        }
        // Draw every player EXCEPT the local one. In first-person the local
        // player's own collision box would render directly around the camera
        // (you'd see your own "body" as a box at your feet), so the caller
        // passes its index to skip it; other players (e.g. test bots, or
        // remote players in multiplayer) are drawn normally.
        for (int i = 0; i < (int)players.size(); ++i) {
            if (i == localPlayerIndex) {
                // First-person: own body is skipped, but the reticle still draws
                // as the player's crosshair, floating out along their aim.
                if (players[i].reticle.isVisibleToOwner) DrawReticle(players[i]);
                continue;
            }
            DrawPlayer(players[i]);
            // Reticle shows others where this player is looking.
            if (players[i].reticle.isVisibleToEnemies) DrawReticle(players[i]);
        }
        for (Asteroid& asteroid : asteroids) {
            DrawAsteroid(asteroid);
        }
        for (Rocket& rocket : rockets) {
            DrawRocket(rocket);
        }
        for (Explosion& explosion : explosions) {
            DrawExplosion(explosion);
        }
        for (Spark& spark : sparks) {
            DrawSpark(spark);
        }
    }

    //MARK: Accessors
    // Collision detection/response (collisions.h) and other external systems
    // need direct access to these vectors. Returned by reference so callers
    // can both iterate and mutate (e.g. taking damage, marking destroyed,
    // pushing new rockets/explosions) without GameSpace exposing raw members.
    Walls& getWalls() { return walls; }
    std::vector<Platform>& getPlatforms() { return platforms; }
    std::vector<Asteroid>& getAsteroids() { return asteroids; }
    std::vector<Player>& getPlayers() { return players; }
    std::vector<Rocket>& getRockets() { return rockets; }
    std::vector<Explosion>& getExplosions() { return explosions; }
    std::vector<Spark>& getSparks() { return sparks; }

private:
    // Mitchell's best-candidate sampling: returns a point that is well-spread
    // relative to already-placed points. Draws `samples` random candidates in
    // the buffer cube and keeps the one farthest from its nearest neighbour.
    // Parameter-free spread (vs. uniform random clustering); always returns a
    // point, unlike min-distance rejection which can fail for large N.
    Vector3 bestCandidatePosition(const std::vector<Vector3>& placed,
                                  float buffer, int samples = 10) {
        Vector3 best{};
        float bestNearestSqr = -1.0f;
        for (int s = 0; s < samples; ++s) {
            Vector3 c{RandomFloat(-buffer, buffer),
                      RandomFloat(-buffer, buffer),
                      RandomFloat(-buffer, buffer)};
            if (placed.empty()) return c; // first point: nothing to space against
            float nearestSqr = std::numeric_limits<float>::max();
            for (const Vector3& p : placed) {
                nearestSqr = std::min(nearestSqr, Vector3DistanceSqr(c, p));
            }
            if (nearestSqr > bestNearestSqr) {
                bestNearestSqr = nearestSqr;
                best = c;
            }
        }
        return best;
    }

    Walls walls; // the play-space boundary cube (single instance)
    int num_of_platforms = GAMESPACE_NUMBER_OF_PLATFORMS;
    std::vector<Platform> platforms;
    int num_of_asteroids = GAMESPACE_NUMBER_OF_ASTEROIDS;
    std::vector<Asteroid> asteroids;
    int number_of_players = GAMESPACE_NUMBER_OF_PLAYERS; // For future use: if we want to add multiplayer support,
    // we can increase this and add a vector of Player objects.
    std::vector<Player> players;
    int number_of_rockets = 0; // Rockets will be generated when the player shoots.
    std::vector<Rocket> rockets;
    int number_of_explosions = 0; // Explosions will be generated when rockets detonate.
    std::vector<Explosion> explosions;
    std::vector<Spark> sparks; // VFX particles (jetpack exhaust, asteroid bursts); visual only.
};
