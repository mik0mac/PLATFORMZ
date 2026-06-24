#include <vector>
#include "collisions.h"
#include "constants.h"

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
        if (player->fuel > 100.0f) player->fuel = 100.0f; // Clamp to max fuel
    }
}

void awardAmmo(Player* player, int ammo) {
    if (player && player->isAlive) {
        player->ammo += ammo;
        if (player->ammo > 100) player->ammo = 100; // Clamp to max ammo
    }
}

void awardHealth(Player* player, int health) {
    if (player && player->isAlive) {
        player->health += health;
        if (player->health > 100) player->health = 100; // Clamp to max health
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
// detonation. Platforms aren't bucketed in the grid (static and few), so they
// are brute-forced like CheckPlayerPlatformCollisions.
void CheckRocketPlatformCollisions(GameSpace& space, const CollisionGrid& grid) {
    (void)grid; // platforms not bucketed - brute-force, see note above

    auto& rockets = space.getRockets();
    auto& platforms = space.getPlatforms();
    auto& explosions = space.getExplosions();

    for (Rocket& rocket : rockets) {
        if (rocket.isDestroyed) continue;

        for (Platform& platform : platforms) {
            // Rocket treated as a small sphere (its size.x), same as the
            // rocket-vs-asteroid check.
            if (SphereIntersectsBox(rocket.position, rocket.size, platform.position, platform.size)) {
                rocket.isDestroyed = true;

                Explosion explosion = spawnExplosion(rocket.position, rocket.owner);
                explosions.push_back(explosion);

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
    float halfSize = space.halfSize;

    for (Rocket& rocket : rockets) {
        if (rocket.isDestroyed) continue;

        bool hitWall = rocket.position.x > halfSize || rocket.position.x < -halfSize ||
                       rocket.position.y > halfSize || rocket.position.y < -halfSize ||
                       rocket.position.z > halfSize || rocket.position.z < -halfSize;
        if (!hitWall) continue;

        // Clamp onto the wall so the explosion appears at the impact point.
        rocket.position.x = Clamp(rocket.position.x, -halfSize, halfSize);
        rocket.position.y = Clamp(rocket.position.y, -halfSize, halfSize);
        rocket.position.z = Clamp(rocket.position.z, -halfSize, halfSize);
        rocket.isDestroyed = true;

        Explosion explosion = spawnExplosion(rocket.position, rocket.owner);
        explosions.push_back(explosion);
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

                // Rocket treated as a small sphere (its size), player as a box,
                // same pairing as the rocket-vs-platform check.
                if (SphereIntersectsBox(rocket.position, rocket.size, player.position, player.size)) {
                    rocket.isDestroyed = true;

                    Explosion explosion = spawnExplosion(rocket.position, rocket.owner);
                    explosions.push_back(explosion);

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

                if (SphereIntersectsBox(asteroid.position, asteroid.size, player.position, player.size)) {
                    player.takeDamage(asteroid.damage);
                    asteroid.isDestroyed = true; // asteroid breaks apart on impact with player
                }
            }
        });
    }
}

//MARK: Asteroid vs Platform
// A drifting asteroid bounces off a platform, reflecting its velocity and being
// pushed clear of the overlap - consistent with how asteroids bounce off the
// boundary walls (reusing space.wall_elasticity, so they keep drifting at a
// constant speed). Platforms aren't bucketed in the grid (static and few), so
// they are brute-forced like CheckPlayerPlatformCollisions.
void CheckAsteroidPlatformCollisions(GameSpace& space, const CollisionGrid& grid) {
    (void)grid; // platforms not bucketed - brute-force, see note above

    auto& asteroids = space.getAsteroids();
    auto& platforms = space.getPlatforms();

    for (Asteroid& asteroid : asteroids) {
        if (asteroid.isDestroyed) continue;

        for (Platform& platform : platforms) {
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

            asteroid.velocity = Vector3Scale(Vector3Reflect(asteroid.velocity, normal), space.wall_elasticity);
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
    (void)grid; // platforms are static and few (default 8) - brute-force
                // checking all of them is simpler and cheap enough that
                // grid-bucketing isn't worth it here. Kept the grid param
                // for signature consistency with the other Check___ functions.

    for (Player& player : players) {
        if (!player.isAlive) continue;

        for (Platform& platform : platforms) {
            if (SphereIntersectsBox(player.position, 0.0f, platform.position, Vector3Add(platform.size, player.size))) {
                // Only resolve when the player is moving down into the platform.
                // This lets the player pass up through it from below, and -
                // critically - stops the check from re-firing every frame while
                // overlapping, which previously flipped velocity.y back and
                // forth and killed the bounce.
                if (player.velocity.y < 0.0f) {
                    float platformTop = platform.position.y + (platform.size.y / 2.0f);
                    // Pop the player out onto the surface first, so next frame
                    // doesn't re-detect the overlap and cancel the response.
                    player.position.y = platformTop + (player.size.y / 2.0f);

                    if (platform.isBouncy) {
                        player.velocity.y = -player.velocity.y * platform.elasticity; // bounce up
                    } else {
                        player.velocity.y = 0.0f; // land and stop
                    }
                }
                // un-comment to work on bouncing off the underside of a platform.
                // else if (player.velocity.y > 0.0f) {
                //     // If the player is moving up into the platform, the should cancel out y velocity.
                //     float platformBottom = platform.position.y - (platform.size.y / 2.0f);
                //     // Pop the player out below the platform to prevent re-detection next frame.
                //     player.position.y = platformBottom - (player.size.y / 2.0f);
                //     if (platform.isBouncy) {
                //         player.velocity.y = -player.velocity.y * platform.elasticity; // bounce down
                //     } else {
                //         player.velocity.y = 0.0f; // cancel upward velocity when hitting the underside
                //     }

                // }
            }
        }
    }
}

//MARK: Player vs Walls
// GameSpace is a cube from -halfSize to +halfSize on each axis. Reflect
// velocity and apply wall_damage on impact, per GameSpace's existing
// wall_elasticity/wall_damage fields.
void CheckPlayerWallCollisions(GameSpace& space) {
    auto& players = space.getPlayers();
    float halfSize = space.halfSize;

    for (Player& player : players) {
        if (!player.isAlive) continue;

        bool hitWall = false;

        // Inset each bound by the player's extent on that axis so the whole
        // player stays inside the cube, not just its center. The +Y (ceiling)
        // bound uses the larger of the box half-height and the eye height, so
        // neither the head nor the first-person camera pokes through the top.
        float halfX = player.size.x * 0.5f;
        float halfY = player.size.y * 0.5f;
        float halfZ = player.size.z * 0.5f;
        float topExtent = player.eyeHeight > halfY ? player.eyeHeight : halfY;

        float xMin = -halfSize + halfX, xMax = halfSize - halfX;
        float yMin = -halfSize + halfY, yMax = halfSize - topExtent;
        float zMin = -halfSize + halfZ, zMax = halfSize - halfZ;

        if (player.position.x < xMin || player.position.x > xMax) {
            player.position.x = Clamp(player.position.x, xMin, xMax);
            player.velocity.x = -player.velocity.x * space.wall_elasticity;
            hitWall = true;
        }
        if (player.position.y < yMin || player.position.y > yMax) {
            player.position.y = Clamp(player.position.y, yMin, yMax);
            player.velocity.y = -player.velocity.y * space.wall_elasticity;
            hitWall = true;
        }
        if (player.position.z < zMin || player.position.z > zMax) {
            player.position.z = Clamp(player.position.z, zMin, zMax);
            player.velocity.z = -player.velocity.z * space.wall_elasticity;
            hitWall = true;
        }

        // if (hitWall) {
        //     player.takeDamage(space.wall_damage);
        // }
    }
}

//MARK: Asteroid vs Walls
// Same boundary cube as CheckPlayerWallCollisions, reusing GameSpace's
// wall_elasticity. Asteroids take no wall_damage (they're not the player).
void CheckAsteroidWallCollisions(GameSpace& space) {
    auto& asteroids = space.getAsteroids();
    float halfSize = space.halfSize;

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
            asteroid.velocity.x = -asteroid.velocity.x * space.wall_elasticity;
        }
        if (asteroid.position.y < minBound || asteroid.position.y > maxBound) {
            asteroid.position.y = Clamp(asteroid.position.y, minBound, maxBound);
            asteroid.velocity.y = -asteroid.velocity.y * space.wall_elasticity;
        }
        if (asteroid.position.z < minBound || asteroid.position.z > maxBound) {
            asteroid.position.z = Clamp(asteroid.position.z, minBound, maxBound);
            asteroid.velocity.z = -asteroid.velocity.z * space.wall_elasticity;
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
            if (dist >= explosion.damageRadius) continue;

            float falloff = 1.0f - (dist / explosion.damageRadius);
            int splashDamage = (int)(explosion.damage * falloff);
            asteroid.takeDamage(splashDamage);
            if (asteroid.isDestroyed) {
                awardPoints(explosion.owner, ASTEROID_SCORE_VALUE); // award points to the player who caused the explosion.
                awardFuel(explosion.owner, ASTEROID_FUEL_AWARD); // award fuel to the player who caused the explosion.
                awardAmmo(explosion.owner, ASTEROID_AMMO_AWARD); // award ammo to the player who caused the explosion.
                awardHealth(explosion.owner, ASTEROID_HEALTH_AWARD); // award health to the player who caused the explosion.
            }

            // Also apply a pushback force to the asteroid, scaled by the same falloff and explosion damage,
            // and inversely by asteroid size (smaller = more push). Push directly away from the explosion center.
            Vector3 pushback = asteroid.position - explosion.position;
            // normalize so the magnitude of the pushback is consistent regardless of distance.
            pushback = Vector3Normalize(pushback);
            // scale by falloff and explosion damage.
            pushback = Vector3Scale(pushback, falloff * explosion.damage * explosion.pushbackFactor);
            asteroid.velocity = Vector3Add(asteroid.velocity, pushback);
        }

        for (Player& player : players) {
            if (!player.isAlive) continue;

            float dist = Vector3Distance(explosion.position, player.position);
            if (dist >= explosion.damageRadius) continue;

            float falloff = 1.0f - (dist / explosion.damageRadius);
            int splashDamage = (int)(explosion.damage * falloff);
            player.takeDamage(splashDamage);
            // check if player has been eliminated.
            if (!player.isAlive) {
                awardPoints(explosion.owner, PLAYER_ELIMINATION_SCORE_VALUE); // award points to the player who caused the explosion.
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
    CheckAsteroidPlatformCollisions(space, grid);
    CheckPlayerPlatformCollisions(space, grid);
    CheckPlayerWallCollisions(space);
    CheckAsteroidWallCollisions(space);
}
