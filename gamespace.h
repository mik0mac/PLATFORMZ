#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <cmath>

#include <algorithm>
#include <limits>

#include "elements.h"
#ifndef PLATFORMZ_SERVER
#include "shapes.h"   // rendering only - excluded from the headless server build
#endif
#include "random.h"
#include "constants.h"


//MARK: NetAudioEvent
// One queued sound event, source-tagged. `fx` is an AudioFXId; `owner` is the
// attributed player's id (0 = world/none, safe since player ids start at 1).
// Filled by the sim (server, or local host), serialized in the state packet,
// and replayed on the client (which skips events it already predicted locally).
struct NetAudioEvent {
    int      fx;
    Vector3  pos;
    uint32_t owner;
};

//MARK: GameSpace
class GameSpace {
public:
    float boundaryBufferPlatforms = 0.8f; // Buffer distance to keep platforms from spawning right on the boundary.  As a factor of walls.halfSize.
    float boundaryBufferAsteroids = 0.9f; // Buffer distance to keep objects from spawning right on the boundary.  As a factor of walls.halfSize.
    Color backgroundColor = {0, 0, 0, 255}; // Black background for the game space
    
    uint32_t nextID = NON_PLAYER_ID_BASE; // Platforms and Asteroids (0x00000100) 256, 257, 258, ... 
    uint32_t nextPlayerID = PLAYER_ID_BASE; // (0x00000001) 1, 2, 3, ... 255.
    // Rockets' IDs generated in input.h


    // Wipe every object vector without repopulating. Used when returning to the
    // title screen so a fresh run (local generate() or a networked reconnect)
    // doesn't inherit stale objects - players in particular are never erased by
    // the netcode (syncByIdNoErase), so they'd otherwise linger across sessions.
    void clear() {
        platforms.clear();
        asteroids.clear();
        players.clear();
        rockets.clear();
        explosions.clear();
        sparks.clear();
        audioEvents.clear();
    }

    // Generate the static/dynamic WORLD (platforms + asteroids) and clear the
    // transient object pools - but NOT players. Split out of generate() so the
    // networked server can (re)build a fresh world for a match while keeping the
    // existing player slots (and their ids) stable across a restart.
    void generateWorld() {
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
        rockets.clear();    // Rockets will be generated when the player shoots.
        explosions.clear(); // Explosions will be generated when rockets detonate.
        sparks.clear();     // VFX particles, spawned by jetpack exhaust / asteroid bursts.
    }

    // Spawn the player at a fresh start point: on top of a random platform (or
    // the origin if none) facing the center, velocity zeroed. Shared by the
    // initial spawn (spawnPlayers) and a match reset (resetPlayersForMatch).
    void placePlayer(Player& player) {
        if (!platforms.empty()) {
            int platformIndex = static_cast<int>(RandomFloat(0, platforms.size() - 1));
            Platform& startPlatform = platforms[platformIndex];
            player.position = Vector3Add(startPlatform.position, Vector3{0, startPlatform.size.y / 2 + player.radius, 0}); // on top of the platform
        } else {
            player.position = {0.0f, 0.0f, 0.0f}; // no platforms (lobby): start at the center
        }
        player.startingPosition = player.position;
        // Face the center of the game space.
        Vector3 toCenter = Vector3Subtract({0, 0, 0}, player.position);
        player.yaw = atan2f(toCenter.z, toCenter.x); // angle in the XZ plane
        player.pitch = 0.0f; // look horizontally
        player.velocity = {0.0f, 0.0f, 0.0f};
    }

    // Create the player slots (fresh ids). Mode-neutral: makes no bot
    // assumptions, since this is shared by the local client AND the authoritative
    // server. Bot-ness is owned by whoever runs the sim - local mode marks/colors
    // its wander-bots in main.cpp's startGame; networked play takes isBot from the
    // server over the wire. Used by generate() and, with no world yet, by the
    // server's lobby boot.
    void spawnPlayers() {
        players.clear();
        for (int i = 0; i < number_of_players; ++i) {
            Player player;
            //MARK: Player ID
            player.id = nextPlayerID++;
            //MARK: Player Color
            //assign colors to players in a round-robin fashion from the HUMAN_PLAYER_COLORS vector
            player.color_outline = HUMAN_PLAYER_COLORS[i % HUMAN_PLAYER_COLORS.size()];
            Color fill = player.color_outline;
            fill.a = 40; // low alpha translucent fill for the "glowing vector glass" effect
            player.color_fill = fill;

            placePlayer(player);
            players.push_back(player);
        }
    }

    void generate() {
        generateWorld();
        spawnPlayers();
    };

    // Reset the EXISTING player slots for a fresh match without changing their
    // ids/count (Player isn't copy-assignable - const members - so reset fields
    // individually). Keeps the server's slot<->client mapping stable across a
    // restart. Call after generateWorld() so placePlayer has platforms to use.
    void resetPlayersForMatch() {
        for (Player& p : players) {
            p.health  = PLAYER_STARTING_HEALTH;
            p.fuel    = PLAYER_STARTING_FUEL;
            p.ammo    = PLAYER_STARTING_AMMO;
            p.isAlive = true;
            p.score   = 0;
            p.deathBurstSpawned = false;
            p.flashTimer    = 0.0f;
            p.coolDownTime  = 0.0f;
            p.isUsingJetpack = false;
            placePlayer(p); // reposition on the new platforms + zero velocity/orientation
        }
        rockets.clear();
        explosions.clear();
        sparks.clear();
    }

    // MARK: Update Objects
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

    // Advance each asteroid's tumble angle. Runs every frame in BOTH local and
    // networked modes (asteroid.velocity is synced on net clients), replacing the old
    // GetTime()*speed formula in DrawAsteroidShape that snapped on bounce.
    void updateAsteroidSpin(float dt) {
        auto hash01 = [](float a, float b, float c) {
            float h = sinf(a * 12.9898f + b * 78.233f + c * 37.719f) * 43758.5453f;
            return h - floorf(h);
        };
        for (Asteroid& a : asteroids) {
            float rate = 0.3f + 0.7f * hash01(a.startingPosition.x, a.startingPosition.z, 4.0f);
            rate *= Vector3Length(a.velocity) / ASTEROID_MIN_SPEED;
            a.spinAngle += rate * dt;
        }
    }

    // The standard one-time spherical spark burst spawned when an asteroid breaks
    // apart or a player is eliminated. Centralized so the local sim
    // (updateActiveObjects) and the networked client (main.cpp) spawn an
    // identical effect. Visual only - pushes into the sparks vector.
    void spawnEliminationBurst(Vector3 at, Color color = {255, 180, 60, 255}) { // hot orange break-up
        Color burst = color; // hot orange break-up
        SpawnSparkBurst(sparks, at, ASTEROID_BURST_SPEED_MIN, ASTEROID_BURST_SPEED_MAX,
                        ASTEROID_BURST_COUNT, burst,
                        SPARK_BURST_LIFETIME_MIN, SPARK_BURST_LIFETIME_MAX);
    }

    // Tick the VFX sparks (drift + fade) and drop dead ones, nothing else. The
    // networked client uses this because the server owns every other object's
    // lifecycle, but sparks are a purely local effect that is never serialized -
    // so the client must integrate and retire them on its own.
    void updateSparks(float dt) {
        for (Spark& spark : sparks) {
            spark.update(dt);
        }
        sparks.erase(std::remove_if(sparks.begin(), sparks.end(),
            [](const Spark& spark) { return !spark.isActive; }), sparks.end());
    }

    void updateActiveObjects() {
        // Spawn a one-time spherical spark burst for each asteroid about to be
        // removed. Done here so it fires regardless of what destroyed it (rocket
        // splash, player impact, ...) - a single centralized hook.
        for (const Asteroid& asteroid : asteroids) {
            if (asteroid.isDestroyed) {
                spawnEliminationBurst(asteroid.position);
                emitAudio(FX_ASTEROID_BREAK, asteroid.position); // owner 0 = world
            }
        }

        // One-time spark burst when a player is eliminated. Players aren't erased
        // (the local player drives the death screen), so a per-player flag keeps
        // the burst from re-firing every frame.
        for (Player& player : players) {
            if (!player.isAlive && !player.deathBurstSpawned) {
                spawnEliminationBurst(player.position, player.color_outline);
                emitAudio(FX_PLAYER_DEATH, player.position, player.id);
                player.deathBurstSpawned = true;
            }
        }

        // MARK: Remove Objects
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
    // Rendering - excluded from the headless server build (PLATFORMZ_SERVER),
    // which never calls draw() and has no raylib draw primitives available.
    // Must be called between BeginMode3D(camera)/EndMode3D() in main.cpp -
    // GameSpace itself doesn't own or know about the camera, that's an
    // orchestration concern that belongs at the main.cpp level.
#ifndef PLATFORMZ_SERVER
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
            // Multiplayer: skip unoccupied slots entirely (no body, no reticle).
            // Always true in local mode, so bots and the local human still draw.
            if (!players[i].isConnected) continue;
            if (!players[i].isAlive) continue; // Dead players don't draw (no body, no reticle).
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
#endif // PLATFORMZ_SERVER

    //MARK: Accessors
    // Collision detection/response (collisions.h) and other external systems
    // need direct access to these vectors. Returned by reference so callers
    // can both iterate and mutate (e.g. taking damage, marking destroyed,
    // pushing new rockets/explosions) without GameSpace exposing raw members.
    // Set the boundary size and object density before generate() - used by the
    // title screen's map-size presets (small/medium/large). Local play only;
    // networked games take their world from the server.
    void configureMap(float halfSize, int platforms, int asteroids) {
        walls.halfSize   = halfSize;
        num_of_platforms = platforms;
        num_of_asteroids = asteroids;
    }

    Walls& getWalls() { return walls; }
    std::vector<Platform>& getPlatforms() { return platforms; }
    std::vector<Asteroid>& getAsteroids() { return asteroids; }
    std::vector<Player>& getPlayers() { return players; }
    std::vector<Rocket>& getRockets() { return rockets; }
    std::vector<Explosion>& getExplosions() { return explosions; }
    std::vector<Spark>& getSparks() { return sparks; }
    std::vector<NetAudioEvent>& getAudioEvents() { return audioEvents; }

    // Queue a sound event at the source of some game action. Filled by the sim
    // (server, or local host); the server serializes these in the state packet
    // and clears them each tick, the local host drains them straight into the
    // client's AudioQueue. owner = the attributed player's id (0 = world/none).
    void emitAudio(int fx, Vector3 at, uint32_t owner = 0) {
        audioEvents.push_back({fx, at, owner});
    }

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
    // int number_of_rockets = 0; // Rockets will be generated when the player shoots.
    std::vector<Rocket> rockets;
    // int number_of_explosions = 0; // Explosions will be generated when rockets detonate.
    std::vector<Explosion> explosions;
    std::vector<Spark> sparks; // VFX particles (jetpack exhaust, asteroid bursts); visual only.
    std::vector<NetAudioEvent> audioEvents; // sound events this tick; serialized + cleared by the server, drained by the client.
};
