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
#include "messages.h"


//MARK: NetAudioEvent
// One queued sound event, source-tagged. `fx` is an AudioFXId; `owner` is the
// attributed player's id (0 = world/none, safe since player ids start at 1).
// Filled by the sim (server, or local host), serialized in the state packet,
// and replayed on the client (which skips events it already predicted locally).
struct NetAudioEvent {
    int      fx;
    Vector3  pos;
    uint32_t owner;
    float    volumeScale;
};


//MARK: GameSpace
class GameSpace {
public:
    float boundaryBufferPlatforms = 0.8f; // Buffer distance to keep platforms from spawning right on the boundary.  As a factor of walls.halfSize.
    float boundaryBufferAsteroids = 0.9f; // Buffer distance to keep objects from spawning right on the boundary.  As a factor of walls.halfSize.
    Color backgroundColor = {0, 0, 0, 255}; // Black background for the game space

    // Player-selectable gameplay rules (set from the OPTIONS modal before a match;
    // networked play threads them via serializeStart -> server). Default to the
    // compile-time constants so an unconfigured GameSpace behaves as before.
    bool wallsStopRockets = WALLS_STOP_ROCKETS;                                    // rockets detonate on the boundary wall vs fly through
    bool earthGravityPassThroughPlatforms = EARTH_GRAVITY_PASS_THROUGH_PLATFORMS;  // under earth gravity, fall through platforms vs land on them
    
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

    // Generate the static WORLD platforms and clear the transient object pools -
    // but NOT players or asteroids. Split out so the generation order is
    // platforms -> players (spread) -> asteroids (buffered away from players),
    // per issue #5; asteroids need the player positions, so they're generated
    // separately (generateAsteroids) AFTER players are placed. The networked
    // server calls this to (re)build a fresh world while keeping the existing
    // player slots (and their ids) stable across a restart.
    void generatePlatforms() {
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
        
        rockets.clear();    // Rockets will be generated when the player shoots.
        explosions.clear(); // Explosions will be generated when rockets detonate.
        sparks.clear();     // VFX particles, spawned by jetpack exhaust / asteroid bursts.
    }

    // Scatter the asteroids randomly, keeping each at least
    // ASTEROID_PLAYER_SPAWN_BUFFER from every already-placed player (issue #5).
    // Call AFTER placePlayersSpread so player positions are known. Draws a few
    // random candidates in the buffer cube and keeps the first that clears all
    // players, falling back to the farthest-from-nearest-player candidate after
    // `samples` tries so a crowded map never hard-fails.
    void generateAsteroids() {
        asteroids.clear();
        float buffer = walls.halfSize * boundaryBufferAsteroids;
        for (int i = 0; i < num_of_asteroids; ++i) {
            Asteroid asteroid;
            //MARK: Asteroid ID
            asteroid.id = nextID++;

            // size, position, and velocity are generated randomly:
            asteroid.generateSize(); // Random size for the asteroid
            asteroid.position = asteroidPositionAwayFromPlayers(buffer);
            asteroid.startingPosition = asteroid.position; // Store the initial position of the asteroid
            asteroid.generateVelocity(); // Random velocity in each direction
            asteroids.push_back(asteroid);
        }
    }

    // Spawn the player at a fresh start point: on top of a given platform-top
    // position (or the origin if none), facing the center, velocity zeroed. The
    // caller chooses the position (placePlayersSpread spreads them out); this
    // just finishes the per-player state. Shared by the initial spawn and match
    // reset so both go through the same finish.
    void orientPlayerAt(Player& player, Vector3 pos) {
        player.position = pos;
        player.startingPosition = player.position;
        // Face the center of the game space.
        Vector3 toCenter = Vector3Subtract({0, 0, 0}, player.position);
        player.yaw = atan2f(toCenter.z, toCenter.x); // angle in the XZ plane
        player.pitch = 0.0f; // look horizontally
        player.velocity = {0.0f, 0.0f, 0.0f};
    }

    // Place every player as far apart as possible (issue #5): the first player
    // lands on a random platform top, then each subsequent player takes the
    // platform top that maximizes its minimum distance to the already-placed
    // players (farthest-point / greedy - same spirit as bestCandidatePosition,
    // but over the discrete set of platform tops). No platforms (lobby) -> origin.
    void placePlayersSpread() {
        std::vector<Vector3> chosen;
        chosen.reserve(players.size());
        for (Player& player : players) {
            Vector3 pos;
            if (platforms.empty()) {
                pos = {0.0f, 0.0f, 0.0f}; // no platforms (lobby): start at the center
            } else if (chosen.empty()) {
                int idx = static_cast<int>(RandomFloat(0, platforms.size() - 1));
                pos = platformTop(platforms[idx], player.radius);
            } else {
                // Pick the platform top farthest from its nearest already-placed player.
                float bestNearestSqr = -1.0f;
                pos = platformTop(platforms[0], player.radius);
                for (const Platform& platform : platforms) {
                    Vector3 top = platformTop(platform, player.radius);
                    float nearestSqr = std::numeric_limits<float>::max();
                    for (const Vector3& c : chosen)
                        nearestSqr = std::min(nearestSqr, Vector3DistanceSqr(top, c));
                    if (nearestSqr > bestNearestSqr) { bestNearestSqr = nearestSqr; pos = top; }
                }
            }
            orientPlayerAt(player, pos);
            chosen.push_back(player.position);
        }
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
            //MARK: Player Color (round-robin human colors; bots recolored by the caller)
            assignPlayerColor(player, i);
            // Unique default name per slot so un-named humans read distinctly
            // ("PLAYER 1", "PLAYER 2", ...). Custom names + bot names overwrite this.
            player.name = "PLAYER " + std::to_string(i + 1);

            players.push_back(player);
        }
        placePlayersSpread(); // spread all slots across the platform tops (issue #5)
    }

    // MARK: GENERATE WORLD
    // Local sim: build the whole world in the issue-#5 order - platforms first,
    // then players spread across them, then asteroids scattered away from players.
    void generate() {
        generatePlatforms();
        spawnPlayers();     // creates slots + spreads them across the platforms
        generateAsteroids(); // buffered away from the now-placed players

        // for a single player game, the platform elasticity for asteroids should be set to 1.0f
        // so that asteroids don't slow down.  Wall elasticity is set > 1.0 so that asteroids
        // speed up gradually over time, making the game more challenging.
        // MARK: Single Player Elasticity
        if (players.size() == 1) {
            for (Platform& platform : platforms) {
                platform.elasticityAsteroid = 1.0f;
            }
        }
        if (players.size() == 1) walls.elasticityAsteroid = 1.033f; else walls.elasticityAsteroid = WALL_ELASTICITY_ASTEROID;  // for a single player game, the walls elasticity for asteroids should be set to 1.0f so that asteroids don't slow down.
    };

    // Reset the EXISTING player slots for a fresh match without changing their
    // ids/count (Player isn't copy-assignable - const members - so reset fields
    // individually). Keeps the server's slot<->client mapping stable across a
    // restart. Call after generatePlatforms() so placePlayersSpread has platforms
    // to spread the slots across; follow with generateAsteroids().
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
            p.isSpectating   = false;                 // fresh match: no one is spectating yet
            p.SpectatingTimer = p.countdownToSpectating; // re-arm the post-death spectate delay
        }
        placePlayersSpread(); // reposition all slots spread across the new platforms (issue #5)
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

    // Networked-only: dead-reckon between server snapshots. The client renders
    // discrete 60Hz snapshots; without this, motion freezes on gap frames and
    // jumps on double frames (visible stepping). Advancing each authoritative
    // mover by its (synced) velocity fills those gaps; the next snapshot re-bases
    // position, so there's no drift (a stationary object has zero velocity, hence
    // zero extrapolation). Position ONLY - it must not touch the per-object timers
    // that updatePos() ticks (flash/cooldown/spectate), since those are
    // server-owned and synced. Explosions/sparks/spin are handled elsewhere.
    void extrapolate(float dt) {
        for (Player& p : players)   p.position = Vector3Add(p.position, Vector3Scale(p.velocity, dt));
        for (Asteroid& a : asteroids) a.position = Vector3Add(a.position, Vector3Scale(a.velocity, dt));
        for (Rocket& r : rockets)   r.position = Vector3Add(r.position, Vector3Scale(r.velocity, dt));
    }

    // Advance each asteroid's tumble angle. Runs every frame in BOTH local and
    // networked modes (asteroid.velocity is synced on net clients), replacing the old
    // GetTime()*speed formula in DrawAsteroidShape that snapped on bounce.
    void updateAsteroidSpin(float dt) {
        for (Asteroid& a : asteroids) {
            float rate = 0.3f + 0.7f * Hash01(a.startingPosition.x, a.startingPosition.z, 4.0f);
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
    // Player bodies + reticles for one draw pass. Skips the local player's own
    // body (first-person: it would render around the camera) but still draws its
    // reticle; skips unoccupied and dead slots. Shared by both passes so the
    // skip rules can't drift apart.
    void drawPlayersPass(int localPlayerIndex, DrawPass pass) {
        for (int i = 0; i < (int)players.size(); ++i) {
            if (!players[i].isConnected) continue; // unoccupied slot: no body, no reticle
            if (!players[i].isAlive) continue;     // dead players don't draw
            if (i == localPlayerIndex) {
                if (players[i].reticle.isVisibleToOwner) DrawReticle(players[i], pass);
                continue; // own body skipped in first-person
            }
            DrawPlayer(players[i], pass);
            if (players[i].reticle.isVisibleToEnemies) DrawReticle(players[i], pass);
        }
    }

    // Two-pass draw (see shapes.h DrawPass): all OPAQUE wireframes first - which
    // builds the depth buffer - then all TRANSLUCENT fills in a single depth-mask-
    // off block. This collapses the per-object batch flushes (~56/frame, the main
    // framerate cost) down to the one flush pair around the fill pass.
    void draw(int localPlayerIndex = -1) {
        // ---- Pass 1: opaque wireframes (write depth) ----
        DrawWalls(walls);
        for (Platform& platform : platforms)   DrawPlatform(platform, PASS_WIRE);
        drawPlayersPass(localPlayerIndex, PASS_WIRE);
        for (Asteroid& asteroid : asteroids)   DrawAsteroid(asteroid, PASS_WIRE);
        for (Rocket& rocket : rockets)         DrawRocket(rocket, PASS_WIRE);
        for (Explosion& explosion : explosions) DrawExplosion(explosion, PASS_WIRE);
        for (Spark& spark : sparks)            DrawSpark(spark); // wire-only lines

        // ---- Pass 2: translucent fills (no depth write), one flush pair ----
        BeginTranslucentFill();
        for (Platform& platform : platforms)   DrawPlatform(platform, PASS_FILL);
        drawPlayersPass(localPlayerIndex, PASS_FILL);
        for (Asteroid& asteroid : asteroids)   DrawAsteroid(asteroid, PASS_FILL);
        for (Rocket& rocket : rockets)         DrawRocket(rocket, PASS_FILL);
        for (Explosion& explosion : explosions) DrawExplosion(explosion, PASS_FILL);
        EndTranslucentFill();
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

    // Set the number of player slots for the next match. Local mode calls this
    // before generate() (spawnPlayers rebuilds the vector to this count). The
    // server calls it in place to grow/shrink WITHOUT dropping connected clients:
    // low-index slots keep their id/state, new slots get a fresh id + round-robin
    // color (same as spawnPlayers), extra slots are popped off the tail. Clamped
    // to >= 1 (always at least the local human / one slot).
    void setPlayerCount(int n) {
        number_of_players = n < 1 ? 1 : n;
        while ((int)players.size() > number_of_players) players.pop_back();
        while ((int)players.size() < number_of_players) {
            Player player;
            player.id = nextPlayerID++;
            assignPlayerColor(player, (int)players.size());
            player.name = "PLAYER " + std::to_string(players.size() + 1); // unique default (see spawnPlayers)
            players.push_back(player);
        }
    }

    Walls& getWalls() { return walls; }
    std::vector<Platform>& getPlatforms() { return platforms; }
    std::vector<Asteroid>& getAsteroids() { return asteroids; }
    std::vector<Player>& getPlayers() { return players; }
    std::vector<Rocket>& getRockets() { return rockets; }
    std::vector<Explosion>& getExplosions() { return explosions; }
    std::vector<Spark>& getSparks() { return sparks; }
    std::vector<NetAudioEvent>& getAudioEvents() { return audioEvents; }
    std::vector<Message>& getMessages() { return messages; }

    // MARK: Audio/Message Events
    // Queue a sound event at the source of some game action. Filled by the sim
    // (server, or local host); the server serializes these in the state packet
    // and clears them each tick, the local host drains them straight into the
    // client's AudioQueue. owner = the attributed player's id (0 = world/none).
    void emitAudio(int fx, Vector3 at, uint32_t owner = 0, float volumeScale = 1.0f) {
        audioEvents.push_back({fx, at, owner, volumeScale});
    }

    void emitMessage(Message msg) {
        messages.push_back(msg);
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

    // Center-top of a platform, offset up by the player's radius so the body
    // sits ON the surface rather than intersecting it. Used for player spawns.
    static Vector3 platformTop(const Platform& p, float playerRadius) {
        return Vector3Add(p.position, Vector3{0, p.size.y / 2 + playerRadius, 0});
    }

    // A random asteroid spawn inside the buffer cube that stays at least
    // ASTEROID_PLAYER_SPAWN_BUFFER from every already-placed player (issue #5).
    // Keeps the first candidate that clears all players; after `samples` misses
    // (crowded map), returns the candidate farthest from its nearest player so
    // it never hard-fails. With no players it's just a uniform random draw.
    Vector3 asteroidPositionAwayFromPlayers(float buffer, int samples = 10) {
        const float minSqr = ASTEROID_PLAYER_SPAWN_BUFFER * ASTEROID_PLAYER_SPAWN_BUFFER;
        Vector3 best{};
        float bestNearestSqr = -1.0f;
        for (int s = 0; s < samples; ++s) {
            Vector3 c{RandomFloat(-buffer, buffer),
                      RandomFloat(-buffer, buffer),
                      RandomFloat(-buffer, buffer)};
            float nearestSqr = std::numeric_limits<float>::max();
            for (const Player& p : players)
                nearestSqr = std::min(nearestSqr, Vector3DistanceSqr(c, p.position));
            if (nearestSqr >= minSqr) return c; // clears every player - take it
            if (nearestSqr > bestNearestSqr) { bestNearestSqr = nearestSqr; best = c; }
        }
        return best; // fallback: farthest-from-nearest-player candidate seen
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
    std::vector<Message> messages;
};
