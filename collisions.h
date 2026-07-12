// Spatial-grid collision detection and response.
//
// Detection = pure geometry (does this sphere/box overlap that one).
// Response  = game-rule reactions (apply damage, mark destroyed, spawn explosion).
// Both live here since for this project's scope they're tightly coupled -
// detection answers "are they touching?", response decides "what happens
// because they're touching?"
//
// Approach: rather than checking every object against every other object
// (O(n^2), wasteful once object counts grow), objects are bucketed into a
// 3D spatial grid each frame. Only objects sharing a cell - or one of its
// 26 neighbors - are ever tested against each other in the narrow phase.

#pragma once

#include "raylib.h"
#include "raymath.h"
#include <vector>
#include <unordered_map>

#include "elements.h"
#include "gamespace.h"

//MARK: Spatial grid
// Cell coordinates are the object's position divided by cellSize and floored.
// Many objects in the same region share the same cell coordinates.
struct CellKey {
    int x, y, z;
    bool operator==(const CellKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

// Hash used only to pick a bucket in the unordered_map below - spatial
// closeness of cells does NOT imply closeness of hash values, and that's
// fine; neighbor lookups always go through real coordinate math (see
// ForEachNeighborCell), never by comparing hashes directly.
struct CellKeyHash {
    size_t operator()(const CellKey& k) const {
        return std::hash<int>()(k.x) ^ (std::hash<int>()(k.y) << 1) ^ (std::hash<int>()(k.z) << 2);
    }
};

// What's stored per cell: indices into GameSpace's object vectors, not
// copies of the objects themselves. Kept separate per object type since
// asteroid-vs-rocket and player-vs-platform are different collision pairs
// with different response logic. Platforms are larger than cellSize, so a
// single platform is bucketed into every cell its AABB overlaps (see
// Rebuild) and the same index appears in multiple cells - queries must
// de-duplicate (see GatherPlatformNeighbors).
struct GridCell {
    std::vector<int> asteroidIndices;
    std::vector<int> rocketIndices;
    std::vector<int> playerIndices;
    std::vector<int> platformIndices;
};

class CollisionGrid {
public:
    float cellSize = 8.0f; // meters per cell; must be >= the largest tested pair
                           // reach so the 27-cell neighbor search can't miss a
                           // contact: asteroid radius (4) + player sphere (2) = 6.

    // Rebuilds the grid from scratch using GameSpace's current object
    // positions. Must be called once per frame, before any collision
    // queries - the grid is fully transient state, not incrementally
    // updated as objects move.
    void Rebuild(GameSpace& space);

    // Calls fn(CellKey) for the cell containing `position` and all 26
    // neighboring cells (27 total, including center).
    template <typename Fn>
    void ForEachNeighborCell(Vector3 position, Fn&& fn) const {
        CellKey center = KeyForPosition(position);
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dz = -1; dz <= 1; dz++) {
                    fn(CellKey{center.x + dx, center.y + dy, center.z + dz});
                }
            }
        }
    }

    const GridCell* FindCell(const CellKey& key) const {
        auto it = cells.find(key);
        return (it != cells.end()) ? &it->second : nullptr;
    }

    // Appends the platform indices bucketed in the 27-cell neighborhood around
    // `position` into `out`, then de-duplicates `out`. Platforms span multiple
    // cells (they're larger than cellSize), so the same index appears in several
    // neighbor cells; callers must not process a platform more than once. Safe to
    // call repeatedly into the same vector (e.g. a rocket's prev + current pos) -
    // the union is re-deduplicated each call.
    void GatherPlatformNeighbors(Vector3 position, std::vector<int>& out) const;

    CellKey KeyForPosition(Vector3 position) const {
        return CellKey{
            (int)floorf(position.x / cellSize),
            (int)floorf(position.y / cellSize),
            (int)floorf(position.z / cellSize)
        };
    }

private:
    std::unordered_map<CellKey, GridCell, CellKeyHash> cells;
};

//MARK: Narrow-phase geometry
// Pure geometry, no game-rule knowledge. Sphere-sphere covers
// asteroid-asteroid and asteroid-rocket (rocket treated as a small sphere
// for detection purposes). Sphere-box covers rocket/asteroid vs player and
// vs platform.
bool SphereIntersectsSphere(Vector3 posA, float radiusA, Vector3 posB, float radiusB);
bool SphereIntersectsBox(Vector3 spherePos, float sphereRadius, Vector3 boxCenter, Vector3 boxSize);
// Swept sphere (radius) travelling p0 -> p1 against an AABB (box expanded by
// radius, slab method). Returns true on overlap and sets tHit to the entry
// parameter in [0,1] (the impact point is Vector3Lerp(p0, p1, tHit)). Used so a
// fast rocket - or one spawned past a thin platform - can't tunnel through.
bool SegmentIntersectsBox(Vector3 p0, Vector3 p1, Vector3 boxCenter, Vector3 boxSize, float radius, float& tHit);

//MARK: Collision response (game rules)
// Each function loops the relevant object pairs using the spatial grid,
// applies narrow-phase checks, and on a hit calls the existing
// takeDamage/isDestroyed methods already defined in elements.h. Spawns
// Explosion objects into GameSpace where appropriate.
void CheckRocketAsteroidCollisions(GameSpace& space, const CollisionGrid& grid);
void CheckRocketPlatformCollisions(GameSpace& space, const CollisionGrid& grid);
void CheckRocketWallCollisions(GameSpace& space);
void CheckRocketPlayerCollisions(GameSpace& space, const CollisionGrid& grid);
void CheckAsteroidPlayerCollisions(GameSpace& space, const CollisionGrid& grid);
void CheckAsteroidPlatformCollisions(GameSpace& space, const CollisionGrid& grid);
void CheckPlayerPlatformCollisions(GameSpace& space, const CollisionGrid& grid);
void CheckPlayerPlayerCollisions(GameSpace& space);
void CheckPlayerWallCollisions(GameSpace& space);
void CheckAsteroidWallCollisions(GameSpace& space);

// Splash damage from active Explosions onto nearby asteroids and players.
// Falloff formula: damage * (1 - distance / damageRadius), so damage is
// maximal at the explosion's center and zero at the edge of damageRadius.
// Applies once per explosion (see Explosion::hasAppliedDamage) - this is a
// single instantaneous blast, not continuous damage while the visual fades.
void ApplyExplosionSplashDamage(GameSpace& space, const CollisionGrid& grid);

// Runs all collision checks for one frame, in order: rebuild grid, then
// each pair-type check. Call this from main.cpp's update step, after
// updatePositions(dt) and before updateActiveObjects().
void RunCollisionChecks(GameSpace& space, CollisionGrid& grid);



