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
                if (SphereIntersectsSphere(rocket.position, rocket.size.x, asteroid.position, asteroid.size)) {
                    // No direct-hit takeDamage call here - ApplyExplosionSplashDamage
                    // handles all damage from the resulting explosion, including the
                    // directly-hit asteroid (it's at ~0 distance from the blast, so
                    // falloff is ~1.0 and it takes ~full damage anyway). Avoids
                    // double-damaging the asteroid this hit produced.
                    rocket.isDestroyed = true;

                    Explosion explosion;
                    explosion.position = rocket.position;
                    explosion.damage = rocket.damage;
                    explosion.damageRadius = rocket.damageRadius;
                    explosions.push_back(explosion);

                    hit = true;
                    break;
                }
            }
        });
    }
}

//MARK: Rocket vs Player
// This is direct rocket-body collision (a rocket physically touching a
// player mid-flight, before detonating) - NOT splash damage, which is
// handled separately by ApplyExplosionSplashDamage and already applies to
// all players in range, including whoever fired the rocket (self-damage
// from your own blast radius is intentional).
// Left as a stub since there's currently no concept of which player fired
// a given rocket (no "owner" field on Rocket) and only one player exists
// by default - revisit if direct rocket-player collision needs distinct
// handling from splash damage once multiplayer or rocket ownership exists.
void CheckRocketPlayerCollisions(GameSpace& space, const CollisionGrid& grid) {
    (void)space;
    (void)grid;
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
            bool falling = player.velocity.y <= 0.0f;
            if (!falling) continue;

            if (SphereIntersectsBox(player.position, 0.0f, platform.position, Vector3Add(platform.size, player.size))) {
                float platformTop = platform.position.y + (platform.size.y / 2.0f);
                player.position.y = platformTop + (player.size.y / 2.0f);
                player.velocity.y = 0.0f;

                if (platform.isBouncy) {
                    player.velocity.y = -player.velocity.y * platform.elasticity;
                }
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

        if (player.position.x > halfSize || player.position.x < -halfSize) {
            player.position.x = Clamp(player.position.x, -halfSize, halfSize);
            player.velocity.x = -player.velocity.x * space.wall_elasticity;
            hitWall = true;
        }
        if (player.position.y > halfSize || player.position.y < -halfSize) {
            player.position.y = Clamp(player.position.y, -halfSize, halfSize);
            player.velocity.y = -player.velocity.y * space.wall_elasticity;
            hitWall = true;
        }
        if (player.position.z > halfSize || player.position.z < -halfSize) {
            player.position.z = Clamp(player.position.z, -halfSize, halfSize);
            player.velocity.z = -player.velocity.z * space.wall_elasticity;
            hitWall = true;
        }

        if (hitWall) {
            player.takeDamage(space.wall_damage);
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
        }

        for (Player& player : players) {
            if (!player.isAlive) continue;

            float dist = Vector3Distance(explosion.position, player.position);
            if (dist >= explosion.damageRadius) continue;

            float falloff = 1.0f - (dist / explosion.damageRadius);
            int splashDamage = (int)(explosion.damage * falloff);
            player.takeDamage(splashDamage);
        }
    }
}

//MARK: RunCollisionChecks
void RunCollisionChecks(GameSpace& space, CollisionGrid& grid) {
    grid.Rebuild(space);

    CheckRocketAsteroidCollisions(space, grid);
    ApplyExplosionSplashDamage(space, grid); // after rocket-asteroid so this-frame explosions resolve immediately
    CheckRocketPlayerCollisions(space, grid);
    CheckAsteroidPlayerCollisions(space, grid);
    CheckPlayerPlatformCollisions(space, grid);
    CheckPlayerWallCollisions(space);
}
