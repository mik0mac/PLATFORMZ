#include <vector>
#include <algorithm>
#include "collisions.h"


//MARK: CollisionGrid::Rebuild
void CollisionGrid::Rebuild(GameSpace& space) {
    cells.clear();

    auto& asteroids = space.getAsteroids();
    for (int i = 0; i < (int)asteroids.size(); i++) {
        CellKey key = KeyForPosition(asteroids[i].position);
        cells[key].asteroidIndices.push_back(i);
    }

    auto& rockets = space.getRockets();
    for (int i = 0; i < (int)rockets.size(); i++) {
        CellKey key = KeyForPosition(rockets[i].position);
        cells[key].rocketIndices.push_back(i);
    }

    auto& players = space.getPlayers();
    for (int i = 0; i < (int)players.size(); i++) {
        CellKey key = KeyForPosition(players[i].position);
        cells[key].playerIndices.push_back(i);
    }

    // Platforms are larger than a cell, so bucket each one into every cell its
    // AABB overlaps - not just its center cell - or the 27-cell neighbor search
    // around an object near the platform's edge could miss it. Rebuilt every
    // frame like the rest of the grid, which also covers future moving platforms.
    auto& platforms = space.getPlatforms();
    for (int i = 0; i < (int)platforms.size(); i++) {
        Vector3 half = Vector3Scale(platforms[i].size, 0.5f);
        CellKey lo = KeyForPosition(Vector3Subtract(platforms[i].position, half));
        CellKey hi = KeyForPosition(Vector3Add(platforms[i].position, half));
        for (int cx = lo.x; cx <= hi.x; cx++)
            for (int cy = lo.y; cy <= hi.y; cy++)
                for (int cz = lo.z; cz <= hi.z; cz++)
                    cells[CellKey{cx, cy, cz}].platformIndices.push_back(i);
    }
}

//MARK: CollisionGrid::GatherPlatformNeighbors
void CollisionGrid::GatherPlatformNeighbors(Vector3 position, std::vector<int>& out) const {
    ForEachNeighborCell(position, [&](const CellKey& key) {
        const GridCell* cell = FindCell(key);
        if (!cell) return;
        out.insert(out.end(), cell->platformIndices.begin(), cell->platformIndices.end());
    });
    // A platform spans multiple cells, so it appears in several of the 27
    // neighbor cells - de-duplicate so callers process each platform once.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

//MARK: Narrow-phase geometry
bool SphereIntersectsSphere(Vector3 posA, float radiusA, Vector3 posB, float radiusB) {
    float distSqr = Vector3DistanceSqr(posA, posB);
    float radiusSum = radiusA + radiusB;
    return distSqr <= (radiusSum * radiusSum);
}

bool SphereIntersectsBox(Vector3 spherePos, float sphereRadius, Vector3 boxCenter, Vector3 boxSize) {
    // Find the closest point on the box to the sphere center, then check
    // if that point is within sphereRadius. boxSize is full width/height/depth,
    // so half-extents are boxSize / 2.
    Vector3 halfExtents = Vector3Scale(boxSize, 0.5f);
    Vector3 boxMin = Vector3Subtract(boxCenter, halfExtents);
    Vector3 boxMax = Vector3Add(boxCenter, halfExtents);

    Vector3 closest{
        Clamp(spherePos.x, boxMin.x, boxMax.x),
        Clamp(spherePos.y, boxMin.y, boxMax.y),
        Clamp(spherePos.z, boxMin.z, boxMax.z)
    };

    float distSqr = Vector3DistanceSqr(spherePos, closest);
    return distSqr <= (sphereRadius * sphereRadius);
}

bool SegmentIntersectsBox(Vector3 p0, Vector3 p1, Vector3 boxCenter, Vector3 boxSize, float radius, float& tHit) {
    // Treat the moving sphere as a point against the box grown by `radius`
    // (Minkowski sum), then ray/slab-clip the segment p0->p1 against that AABB.
    Vector3 halfExtents = Vector3Add(Vector3Scale(boxSize, 0.5f), {radius, radius, radius});
    Vector3 boxMin = Vector3Subtract(boxCenter, halfExtents);
    Vector3 boxMax = Vector3Add(boxCenter, halfExtents);

    // Degenerate (zero-length) segment, or p0 already inside the expanded box:
    // fall back to the static overlap test and report the impact at the start.
    Vector3 d = Vector3Subtract(p1, p0);
    if (Vector3LengthSqr(d) < 1e-12f) {
        tHit = 0.0f;
        return SphereIntersectsBox(p0, radius, boxCenter, boxSize);
    }

    float tMin = 0.0f, tMax = 1.0f;
    const float* o = &p0.x;
    const float* dir = &d.x;
    const float* bMin = &boxMin.x;
    const float* bMax = &boxMax.x;
    for (int axis = 0; axis < 3; axis++) {
        if (fabsf(dir[axis]) < 1e-8f) {
            // Segment is parallel to this slab - reject if it starts outside it.
            if (o[axis] < bMin[axis] || o[axis] > bMax[axis]) return false;
        } else {
            float inv = 1.0f / dir[axis];
            float t1 = (bMin[axis] - o[axis]) * inv;
            float t2 = (bMax[axis] - o[axis]) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tMin) tMin = t1;
            if (t2 < tMax) tMax = t2;
            if (tMin > tMax) return false;
        }
    }

    tHit = tMin;
    return true;
}

//MARK: Spawn Explosion
// avoid repeated code in CheckRocketAsteroidCollisions, CheckRocketPlatformCollisions, and CheckRocketWallCollisions
Explosion spawnExplosion(Vector3 position, Player* owner) {
    Explosion explosion;
    explosion.owner = owner; // track which player fired this rocket
    explosion.position = position;
    return explosion;
}

//MARK: Awards
void awardPoints(Player* player, int points) {
    if (player && player->isAlive) {
        player->score += points;
    }
}

void awardFuel(Player* player, int fuel) {
    if (player && player->isAlive) {
        player->fuel += fuel;
        if (player->fuel > player->maxFuel) player->fuel = player->maxFuel; // Clamp to max fuel
    }
}

void awardAmmo(Player* player, int ammo) {
    if (player && player->isAlive) {
        player->ammo += ammo;
        if (player->ammo > player->maxAmmo) player->ammo = player->maxAmmo; // Clamp to max ammo
    }
}

void awardHealth(Player* player, int health) {
    if (player && player->isAlive) {
        player->health += health;
        if (player->health > player->maxHealth) player->health = player->maxHealth; // Clamp to max health
    }
}


//MARK: Rocket vs Asteroid
// Rocket is the player's projectile - on hit, damages the asteroid and is
// itself destroyed, and spawns an explosion at the impact point.
void CheckRocketAsteroidCollisions(GameSpace& space, const CollisionGrid& grid) {
    auto& rockets = space.getRockets();
    auto& asteroids = space.getAsteroids();
    auto& explosions = space.getExplosions();

    for (Rocket& rocket : rockets) {
        if (rocket.isDestroyed) continue;

        bool hit = false;
        grid.ForEachNeighborCell(rocket.position, [&](const CellKey& key) {
            if (hit) return; // already resolved this rocket this frame
            const GridCell* cell = grid.FindCell(key);
            if (!cell) return;

            for (int asteroidIndex : cell->asteroidIndices) {
                Asteroid& asteroid = asteroids[asteroidIndex];
                if (asteroid.isDestroyed) continue;

                // Rocket treated as a small sphere (its size.x) for detection
                // simplicity, rather than full box-vs-sphere.
                if (SphereIntersectsSphere(rocket.position, rocket.size, asteroid.position, asteroid.size)) {
                    // No direct-hit takeDamage call here - ApplyExplosionSplashDamage
                    // handles all damage from the resulting explosion, including the
                    // directly-hit asteroid (it's at ~0 distance from the blast, so
                    // falloff is ~1.0 and it takes ~full damage anyway). Avoids
                    // double-damaging the asteroid this hit produced.
                    rocket.isDestroyed = true;

                    Explosion explosion = spawnExplosion(rocket.position, rocket.owner);
                    explosions.push_back(explosion);
                    space.emitAudio(FX_EXPLOSION, explosion.position, explosion.owner ? explosion.owner->id : 0);

                    hit = true;
                    break;
                }
            }
        });
    }
}

//MARK: Rocket vs Platform
// A rocket flying into a platform detonates: destroy the rocket and spawn an
// explosion at impact so splash damage hits nearby objects. Platforms have no
// health, so they aren't damaged themselves. Mirrors the rocket-vs-asteroid
// detonation. Candidate platforms come from the grid around both ends of the
// rocket's travel this frame (prevPosition and position) so the swept test still
// can't miss a platform the segment crosses.
void CheckRocketPlatformCollisions(GameSpace& space, const CollisionGrid& grid) {
    auto& rockets = space.getRockets();
    auto& platforms = space.getPlatforms();
    auto& explosions = space.getExplosions();

    for (Rocket& rocket : rockets) {
        if (rocket.isDestroyed) continue;

        std::vector<int> candidates;
        grid.GatherPlatformNeighbors(rocket.prevPosition, candidates);
        grid.GatherPlatformNeighbors(rocket.position, candidates);

        for (int platformIndex : candidates) {
            Platform& platform = platforms[platformIndex];
            // Swept test along the rocket's travel this frame (prevPosition ->
            // position), so a fast rocket - or one spawned past a thin platform
            // on a downward shot - detonates on the platform instead of tunneling.
            float tHit = 0.0f;
            if (SegmentIntersectsBox(rocket.prevPosition, rocket.position, platform.position, platform.size, rocket.size, tHit)) {
                rocket.isDestroyed = true;

                // Detonate at the entry point on the platform, not past it.
                Vector3 impact = Vector3Lerp(rocket.prevPosition, rocket.position, tHit);
                Explosion explosion = spawnExplosion(impact, rocket.owner);
                explosions.push_back(explosion);
                space.emitAudio(FX_EXPLOSION, explosion.position, explosion.owner ? explosion.owner->id : 0);

                break; // rocket already detonated this frame
            }
        }
    }
}

//MARK: Rocket vs Walls
// Same boundary cube as the player/asteroid wall checks. A rocket reaching the
// boundary detonates at the wall (spawns an explosion) and is destroyed, rather
// than escaping the play space. No grid - the boundary test is pure position
// math, matching CheckPlayerWallCollisions / CheckAsteroidWallCollisions.
void CheckRocketWallCollisions(GameSpace& space) {
    auto& rockets = space.getRockets();
    auto& explosions = space.getExplosions();
    float halfSize = space.getWalls().halfSize;

    for (Rocket& rocket : rockets) {
        if (rocket.isDestroyed) continue;

        bool hitWall = rocket.position.x > halfSize || rocket.position.x < -halfSize ||
                       rocket.position.y > halfSize || rocket.position.y < -halfSize ||
                       rocket.position.z > halfSize || rocket.position.z < -halfSize;
        if (!hitWall) continue;

        if (!WALLS_STOP_ROCKETS) {
            space.emitAudio(FX_ROCKET_THROUGH_WALL, rocket.position, rocket.owner ? rocket.owner->id : 0);
            rocket.isOutOfBounds = true; // mark the rocket as out of bounds so it starts to fade.
            continue; // rocket flies through the wall, no detonation
        }

        // Clamp onto the wall so the explosion appears at the impact point.
        rocket.position.x = Clamp(rocket.position.x, -halfSize, halfSize);
        rocket.position.y = Clamp(rocket.position.y, -halfSize, halfSize);
        rocket.position.z = Clamp(rocket.position.z, -halfSize, halfSize);
        rocket.isDestroyed = true;

        Explosion explosion = spawnExplosion(rocket.position, rocket.owner);
        explosions.push_back(explosion);
        space.emitAudio(FX_EXPLOSION, explosion.position, explosion.owner ? explosion.owner->id : 0);
    }
}

//MARK: Rocket vs Player
// This is direct rocket-body collision (a rocket physically touching a
// player mid-flight, before detonating) - NOT splash damage, which is
// handled separately by ApplyExplosionSplashDamage and already applies to
// all players in range, including whoever fired the rocket (self-damage
// from your own blast radius is intentional).
// A rocket physically striking a player mid-flight detonates: destroy the
// rocket and spawn an explosion at the impact point, so the blast's splash
// damage (ApplyExplosionSplashDamage) deals the actual hit - matching the
// rocket-vs-asteroid convention of never calling takeDamage directly. The
// rocket's owner is skipped: a freshly-fired rocket spawns at the firing
// player's position, so without this guard it would instantly detonate on
// the shooter. Now that Rocket carries an owner, other players can be hit.
void CheckRocketPlayerCollisions(GameSpace& space, const CollisionGrid& grid) {
    auto& rockets = space.getRockets();
    auto& players = space.getPlayers();
    auto& explosions = space.getExplosions();

    for (Rocket& rocket : rockets) {
        if (rocket.isDestroyed) continue;

        bool hit = false;
        grid.ForEachNeighborCell(rocket.position, [&](const CellKey& key) {
            if (hit) return; // already resolved this rocket this frame
            const GridCell* cell = grid.FindCell(key);
            if (!cell) return;

            for (int playerIndex : cell->playerIndices) {
                Player& player = players[playerIndex];
                if (!player.isAlive) continue;
                if (&player == rocket.owner) continue; // don't detonate on the shooter

                // Rocket and player are both spheres (the player collider is a
                // sphere matching the dodecahedron body).
                if (SphereIntersectsSphere(rocket.position, rocket.size, player.position, player.radius)) {
                    rocket.isDestroyed = true;

                    Explosion explosion = spawnExplosion(rocket.position, rocket.owner);
                    explosions.push_back(explosion);
                    space.emitAudio(FX_EXPLOSION, explosion.position, explosion.owner ? explosion.owner->id : 0);

                    hit = true;
                    break;
                }
            }
        });
    }
}

//MARK: Asteroid vs Player
void CheckAsteroidPlayerCollisions(GameSpace& space, const CollisionGrid& grid) {
    auto& asteroids = space.getAsteroids();
    auto& players = space.getPlayers();

    for (Player& player : players) {
        if (!player.isAlive) continue;

        grid.ForEachNeighborCell(player.position, [&](const CellKey& key) {
            const GridCell* cell = grid.FindCell(key);
            if (!cell) return;

            for (int asteroidIndex : cell->asteroidIndices) {
                Asteroid& asteroid = asteroids[asteroidIndex];
                if (asteroid.isDestroyed) continue;

                if (SphereIntersectsSphere(asteroid.position, asteroid.size, player.position, player.radius)) {
                    // damage and audio FX.
                    player.takeDamage(asteroid.damage);
                    if (player.isAlive) space.emitAudio(FX_PLAYER_HIT, player.position, player.id);
                    asteroid.takeDamage(player.damage);
                    // if (asteroid.isDestroyed) space.emitAudio(FX_ASTEROID_HIT, asteroid.position, asteroid.id);

                    // velocity bounce: asteroid bounces off the player, player bounces off the asteroid.
                    // The objects need to be pushed clear of each other so they don't re-collide next frame.
                    Vector3 offset = Vector3Subtract(player.position, asteroid.position);
                    float dist = Vector3Length(offset);
                    if (dist > 1e-4f) {
                        Vector3 normal = Vector3Scale(offset, 1.0f / dist);
                        asteroid.velocity = Vector3Scale(Vector3Reflect(asteroid.velocity, normal), 1.0f); // asteroid bounces off the player, same elasticity as the walls
                        player.velocity = Vector3Scale(Vector3Reflect(player.velocity, normal), 1.0f); // player bounces off the asteroid, same elasticity as the walls
                        // push clear of the overlap
                        float overlap = (asteroid.size + player.radius) - dist;
                        asteroid.position = Vector3Subtract(asteroid.position, Vector3Scale(normal, overlap *0.5f));
                        player.position = Vector3Add(player.position, Vector3Scale(normal, overlap *0.5f));
                    }
                }
            }
        });
    }
}

//MARK: Asteroid vs Platform
// A drifting asteroid bounces off a platform, reflecting its velocity and being
// pushed clear of the overlap - consistent with how asteroids bounce off the
// boundary walls (reusing the walls' elasticity, so they keep drifting at a
// constant speed). Candidate platforms come from the spatial grid around the
// asteroid (de-duplicated, since a platform spans several cells).
void CheckAsteroidPlatformCollisions(GameSpace& space, const CollisionGrid& grid) {
    auto& asteroids = space.getAsteroids();
    auto& platforms = space.getPlatforms();

    for (Asteroid& asteroid : asteroids) {
        if (asteroid.isDestroyed) continue;

        std::vector<int> candidates;
        grid.GatherPlatformNeighbors(asteroid.position, candidates);

        for (int platformIndex : candidates) {
            Platform& platform = platforms[platformIndex];
            if (!SphereIntersectsBox(asteroid.position, asteroid.size, platform.position, platform.size)) continue;

            // Closest point on the platform box to the asteroid center (same
            // clamp math SphereIntersectsBox uses internally).
            Vector3 halfExtents = Vector3Scale(platform.size, 0.5f);
            Vector3 boxMin = Vector3Subtract(platform.position, halfExtents);
            Vector3 boxMax = Vector3Add(platform.position, halfExtents);
            Vector3 closest{
                Clamp(asteroid.position.x, boxMin.x, boxMax.x),
                Clamp(asteroid.position.y, boxMin.y, boxMax.y),
                Clamp(asteroid.position.z, boxMin.z, boxMax.z)
            };

            Vector3 offset = Vector3Subtract(asteroid.position, closest);
            float dist = Vector3Length(offset);

            Vector3 normal;
            if (dist > 1e-4f) {
                // Center is outside the box - normal points from surface to center.
                normal = Vector3Scale(offset, 1.0f / dist);
            } else {
                // Center is inside the box (degenerate): push out along the axis
                // of least penetration to the nearest face. For thin platforms
                // this is almost always the Y (top/bottom) axis.
                float penX = halfExtents.x - fabsf(asteroid.position.x - platform.position.x);
                float penY = halfExtents.y - fabsf(asteroid.position.y - platform.position.y);
                float penZ = halfExtents.z - fabsf(asteroid.position.z - platform.position.z);
                if (penX <= penY && penX <= penZ) {
                    normal = {asteroid.position.x < platform.position.x ? -1.0f : 1.0f, 0.0f, 0.0f};
                } else if (penY <= penZ) {
                    normal = {0.0f, asteroid.position.y < platform.position.y ? -1.0f : 1.0f, 0.0f};
                } else {
                    normal = {0.0f, 0.0f, asteroid.position.z < platform.position.z ? -1.0f : 1.0f};
                }
            }

            asteroid.velocity = Vector3Scale(Vector3Reflect(asteroid.velocity, normal), platform.elasticityAsteroid); // bounce off the platform, same elasticity as the walls
            // Push clear of the overlap so it isn't re-detected next frame.
            asteroid.position = Vector3Add(closest, Vector3Scale(normal, asteroid.size));
        }
    }
}

//MARK: Player vs Platform
// Simple "land on top" resolution: if the player's falling and overlapping
// a platform, snap to standing on it and zero out downward velocity.
// Not a full physics resolver - good enough for flat platforms.
void CheckPlayerPlatformCollisions(GameSpace& space, const CollisionGrid& grid) {
    auto& platforms = space.getPlatforms();
    auto& players = space.getPlayers();

    for (Player& player : players) {
        if (!player.isAlive) continue;

        std::vector<int> candidates;
        grid.GatherPlatformNeighbors(player.position, candidates);

        for (int platformIndex : candidates) {
            Platform& platform = platforms[platformIndex];
            if (SphereIntersectsBox(player.position, player.radius, platform.position, platform.size)) {
                // Only resolve when the player is moving down into the platform.
                // This lets the player pass up through it from below, and -
                // critically - stops the check from re-firing every frame while
                // overlapping, which previously flipped velocity.y back and
                // forth and killed the bounce.
                if (player.velocity.y < 0.0f && (player.position.y + player.radius) > (platform.position.y + (platform.size.y / 2.0f))) {
                    // float platformTop = platform.position.y + (platform.size.y / 2.0f);
                    // Pop the player out onto the surface first, so next frame
                    // doesn't re-detect the overlap and cancel the response.
                    // player.position.y = platformTop + player.radius;

                    if (platform.isBouncy) {
                        player.velocity.y = -player.velocity.y * platform.elasticityPlayer; // bounce up
                    } else {
                        player.velocity.y = 0.0f; // land and stop
                    }
                }
                // un-comment to work on bouncing off the underside of a platform.
                // else if (player.velocity.y > 0.0f && (player.position.y - player.radius) < (platform.position.y - (platform.size.y / 2.0f))) {
                //     // If the player is moving up into the platform, the should cancel out y velocity.
                //     float platformBottom = platform.position.y - (platform.size.y / 2.0f);
                //     // Pop the player out below the platform to prevent re-detection next frame.
                //     player.position.y = platformBottom - (player.radius);
                //     if (platform.isBouncy) {
                //         player.velocity.y = -player.velocity.y * platform.elasticityPlayer; // bounce down
                //     } else {
                //         player.velocity.y = 0.0f; // cancel upward velocity when hitting the underside
                //     }

                // }
            }
        }
    }
}

//MARK: Player vs Walls
// The boundary cube spans -halfSize to +halfSize on each axis. Reflect
// velocity and apply the walls' damage on impact, per the Walls element's
// elasticity/damage fields (space.getWalls()).
void CheckPlayerWallCollisions(GameSpace& space) {
    auto& players = space.getPlayers();
    Walls& walls = space.getWalls();
    float halfSize = walls.halfSize;

    for (Player& player : players) {
        if (!player.isAlive) continue;

        bool hitWall = false;

        // Inset each bound by the player's sphere radius so the whole body stays
        // inside the cube, not just its center. The eye is at the sphere center,
        // so the clamp is symmetric on every axis.
        float r = player.radius;

        float xMin = -halfSize + r, xMax = halfSize - r;
        float yMin = -halfSize + r, yMax = halfSize - r;
        float zMin = -halfSize + r, zMax = halfSize - r;

        if (player.position.x < xMin || player.position.x > xMax) {
            player.position.x = Clamp(player.position.x, xMin, xMax);
            player.velocity.x = -player.velocity.x * walls.elasticityPlayer;
            hitWall = true;
        }
        if (player.position.y < yMin || player.position.y > yMax) {
            player.position.y = Clamp(player.position.y, yMin, yMax);
            player.velocity.y = -player.velocity.y * walls.elasticityPlayer;
            hitWall = true;
        }
        if (player.position.z < zMin || player.position.z > zMax) {
            player.position.z = Clamp(player.position.z, zMin, zMax);
            player.velocity.z = -player.velocity.z * walls.elasticityPlayer;
            hitWall = true;
        }

        if (hitWall) {
            player.takeDamage(walls.damage);
            space.emitAudio(FX_WALL_BOUNCE_PLAYER, player.position, player.id);
        }
    }
}

//MARK: Player vs Player
void CheckPlayerPlayerCollisions(GameSpace& space, const CollisionGrid& grid) {
    auto& players = space.getPlayers();

    for (int i = 0; i < (int)players.size(); i++) {
        Player& playerA = players[i];
        if (!playerA.isAlive) continue;

        for (int j = i + 1; j < (int)players.size(); j++) {
            Player& playerB = players[j];
            if (!playerB.isAlive) continue;

            if (SphereIntersectsSphere(playerA.position, playerA.radius, playerB.position, playerB.radius)) {
                // Both players take damage on collision.
                playerA.takeDamage(playerB.damage);
                playerB.takeDamage(playerA.damage);

                if (playerA.isAlive) space.emitAudio(FX_PLAYER_HIT, playerA.position, playerA.id);
                if (playerB.isAlive) space.emitAudio(FX_PLAYER_HIT, playerB.position, playerB.id);

                // Separate the two so they don't stay overlapped and re-damage
                // every frame (mirrors the asteroid-vs-player response). Push
                // each clear by half the overlap along the contact normal, then
                // reflect their velocities so they genuinely bounce apart. A
                // plain velocity bump wasn't enough: it left the closing speed
                // intact, so any real approach speed re-overlapped them next
                // frame and dealt damage every frame (a collision = instant
                // elimination). Reflecting flips the closing component to a
                // separating one, so each collision deals damage exactly once.
                Vector3 offset = Vector3Subtract(playerA.position, playerB.position);
                float dist = Vector3Length(offset);
                if (dist > 1e-4f) {
                    Vector3 normal = Vector3Scale(offset, 1.0f / dist);
                    float overlap = (playerA.radius + playerB.radius) - dist;
                    playerA.position = Vector3Add(playerA.position, Vector3Scale(normal, overlap * 0.5f));
                    playerB.position = Vector3Subtract(playerB.position, Vector3Scale(normal, overlap * 0.5f));

                    // Bounce apart, same elasticity as the walls (matches the asteroid-vs-player bounce).
                    playerA.velocity = Vector3Scale(Vector3Reflect(playerA.velocity, normal), 1.0f);
                    playerB.velocity = Vector3Scale(Vector3Reflect(playerB.velocity, normal), 1.0f);
                }
            }
        }
    }
}

//MARK: Asteroid vs Walls
// Same boundary cube as CheckPlayerWallCollisions, reusing the walls'
// elasticity. Asteroids take no wall damage (they're not the player).
void CheckAsteroidWallCollisions(GameSpace& space) {
    auto& asteroids = space.getAsteroids();
    Walls& walls = space.getWalls();
    float halfSize = walls.halfSize;

    for (Asteroid& asteroid : asteroids) {
        if (asteroid.isDestroyed) continue;

        // Inset each bound by the asteroid's radius so the sphere's edge - not
        // just its center - stays inside the cube, matching how the player
        // wall clamp accounts for the player's extent.
        float r = asteroid.size;
        float minBound = -halfSize + r;
        float maxBound = halfSize - r;

        if (asteroid.position.x < minBound || asteroid.position.x > maxBound) {
            asteroid.position.x = Clamp(asteroid.position.x, minBound, maxBound);
            asteroid.velocity.x = -asteroid.velocity.x * walls.elasticityAsteroid;
        }
        if (asteroid.position.y < minBound || asteroid.position.y > maxBound) {
            asteroid.position.y = Clamp(asteroid.position.y, minBound, maxBound);
            asteroid.velocity.y = -asteroid.velocity.y * walls.elasticityAsteroid;
        }
        if (asteroid.position.z < minBound || asteroid.position.z > maxBound) {
            asteroid.position.z = Clamp(asteroid.position.z, minBound, maxBound);
            asteroid.velocity.z = -asteroid.velocity.z * walls.elasticityAsteroid;
        }
    }
}

//MARK: Explosion splash damage
// Explosions aren't bucketed into the spatial grid (typically few active at
// once), so this loops asteroids/players directly per explosion rather than
// via grid cells - simpler, and cheap enough at this scale.
void ApplyExplosionSplashDamage(GameSpace& space, const CollisionGrid& grid) {
    (void)grid; // unused - see note above

    auto& explosions = space.getExplosions();
    auto& asteroids = space.getAsteroids();
    auto& players = space.getPlayers();

    for (Explosion& explosion : explosions) {
        if (explosion.hasAppliedDamage) continue;
        explosion.hasAppliedDamage = true; // mark immediately - one blast, applied once

        for (Asteroid& asteroid : asteroids) {
            if (asteroid.isDestroyed) continue;

            float dist = Vector3Distance(explosion.position, asteroid.position);
            // subract asteroid radius.
            dist -= asteroid.size;
            if (dist >= explosion.damageRadius) continue;

            float falloff = 1.0f - (dist / explosion.damageRadius);
            int splashDamage = (int)(explosion.damage * falloff);
            asteroid.takeDamage(splashDamage);

            // Also apply a pushback force to the asteroid, scaled by the same falloff and explosion damage,
            // and inversely by asteroid size (smaller = more push). Push directly away from the explosion center.
            Vector3 pushback = asteroid.position - explosion.position;
            // normalize so the magnitude of the pushback is consistent regardless of distance.
            pushback = Vector3Normalize(pushback);
            // scale by falloff and explosion damage.
            pushback = Vector3Scale(pushback, falloff * explosion.damage * explosion.pushbackFactor);
            asteroid.velocity = Vector3Add(asteroid.velocity, pushback);

            // check if the asteroid has been destroyed by the splash damage.
            if (asteroid.isDestroyed) {
                awardPoints(explosion.owner, asteroid.scoreAward); // award points to the player who caused the explosion.
                awardFuel(explosion.owner, asteroid.fuelAward); // award fuel to the player who caused the explosion.
                awardAmmo(explosion.owner, asteroid.ammoAward); // award ammo to the player who caused the explosion.
                awardHealth(explosion.owner, asteroid.healthAward); // award health to the player who caused the explosion.

                // spawn debris from the destroyed asteroid and add it to the game space.  FUTURE.
                // DebrisEffect debris = spawnDebris(asteroid.position, asteroid.velocity); // spawn debris from the destroyed asteroid
                // debrisEffects.push_back(debris); // add the debris to the game space
            }
        }

        for (Player& player : players) {
            if (!player.isAlive) continue;

            float dist = Vector3Distance(explosion.position, player.position);
            // subtract the player's sphere collider radius (matches the body).
            dist -= player.radius;
            if (dist >= explosion.damageRadius) continue;

            float falloff = 1.0f - (dist / explosion.damageRadius);
            int splashDamage = (int)(explosion.damage * falloff);
            player.takeDamage(splashDamage);
            if (player.isAlive) space.emitAudio(FX_PLAYER_HIT, player.position, player.id);
            // check if player has been eliminated.
            if (!player.isAlive) {
                awardPoints(explosion.owner, player.eliminationScoreAward); // award points to the player who caused the explosion.
                // could later add a "player eliminated" event here for UI feedback, etc.
                // could add fuel/ammo/health awards for eliminating a player, if desired.
            }

            // Also apply a pushback force to the player, scaled by the same falloff.
            Vector3 pushback = player.position - explosion.position;
            // normalize so the magnitude of the pushback is consistent regardless of distance.
            pushback = Vector3Normalize(pushback);
            pushback = Vector3Scale(pushback, falloff * explosion.damage * explosion.pushbackFactor); // scale pushback by damage and pushback factor.
            player.velocity = Vector3Add(player.velocity, pushback);
        }
    }
}

//MARK: RunCollisionChecks
void RunCollisionChecks(GameSpace& space, CollisionGrid& grid) {
    grid.Rebuild(space);

    CheckRocketAsteroidCollisions(space, grid);
    CheckRocketPlatformCollisions(space, grid);
    CheckRocketWallCollisions(space);
    CheckRocketPlayerCollisions(space, grid);
    ApplyExplosionSplashDamage(space, grid); // after rocket detonation checks so this-frame explosions resolve immediately
    CheckAsteroidPlayerCollisions(space, grid);
    CheckPlayerPlayerCollisions(space, grid);
    CheckAsteroidPlatformCollisions(space, grid);
    CheckPlayerPlatformCollisions(space, grid);
    CheckPlayerWallCollisions(space);
    CheckAsteroidWallCollisions(space);
}
