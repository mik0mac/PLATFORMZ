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
#include <cstdint>

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
    // NOTE: no platformIndices. Platforms are static, so they are bucketed once
    // into a separate long-lived layer (staticCells) rather than being wiped and
    // re-inserted every frame with everything else. See RebuildStatic.
    // Generation this cell was last filled for. Rebuild() bumps a counter rather
    // than clearing the map, so a cell whose stamp is stale is LOGICALLY EMPTY -
    // FindCell() reports it as absent. Its vectors still hold last generation's
    // indices, and (the whole point) still own their heap buffers, so the next
    // Touch() reuses them instead of asking the allocator for new ones.
    uint32_t stamp = 0;
};

class CollisionGrid {
public:
    float cellSize = 8.0f; // meters per cell; must be >= the largest tested pair
                           // reach so the 27-cell neighbor search can't miss a
                           // contact: asteroid radius (4) + player sphere (2) = 6.

    // Rebuilds the grid using GameSpace's current object positions. Must be
    // called once per frame, before any collision queries - the grid is fully
    // transient state, not incrementally updated as objects move.
    //
    // "Rebuild" no longer means "throw everything away". It used to open with
    // cells.clear(), which destroyed every GridCell and freed all four of its
    // vector buffers, then immediately rebuilt near-identical contents: a
    // platform spans ~4-32 cells at cellSize 8, so an XL map (576 platforms) ran
    // thousands of malloc/free pairs per frame, 60 times a second, per match.
    // Now it bumps a generation counter and lets Touch() wipe each cell in place
    // the first time this frame reaches it. Steady state allocates nothing.
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

    // The static (platform) layer, rebuilt only when the platform layout changes.
    // Empty vector rather than null so callers need no special case.
    const std::vector<int>& FindStaticCell(const CellKey& key) const {
        static const std::vector<int> kNone;
        auto it = staticCells.find(key);
        return it == staticCells.end() ? kNone : it->second;
    }

    // A cell not touched this generation is empty as far as callers are
    // concerned, even though the map entry still exists - see GridCell::stamp.
    const GridCell* FindCell(const CellKey& key) const {
        auto it = cells.find(key);
        if (it == cells.end() || it->second.stamp != generation) return nullptr;
        return &it->second;
    }

    // Live cell count (this generation only) - for tests and instrumentation.
    size_t LiveCellCount() const {
        size_t n = 0;
        for (const auto& [key, cell] : cells) if (cell.stamp == generation) n++;
        return n;
    }
    size_t RetainedCellCount() const { return cells.size(); }

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
    // Fetch a cell for writing this generation, wiping it in place if the last
    // thing that wrote it was an earlier frame. vector::clear() keeps capacity,
    // which is what makes the reuse free.
    GridCell& Touch(const CellKey& key) {
        GridCell& c = cells[key];
        if (c.stamp != generation) {
            c.asteroidIndices.clear();
            c.rocketIndices.clear();
            c.playerIndices.clear();
            c.stamp = generation;
        }
        return c;
    }

    // Reuse keeps buffers alive forever, which is the trade: a cell that saw one
    // rocket fly through it in minute two still owns its buffer in minute twenty.
    // The arena is bounded but big - XL is 720 units across at cellSize 8, ~90
    // cells per axis, so ~729k cells if moving objects eventually visit
    // everywhere. Left alone that is tens of MB per match on a 1 GB box. So drop
    // cells nothing has touched for a while; infrequent enough to be free.
    void EvictStale();

    // Re-bucket every platform into staticCells. Called from Rebuild only when
    // GameSpace::getPlatformEpoch() differs from what was last built, which for a
    // normal match means exactly once, at match start - instead of 576 platforms
    // x their 4-32 cells apiece, 60 times a second, forever (#99).
    void RebuildStatic(GameSpace& space);

    std::unordered_map<CellKey, GridCell, CellKeyHash> cells;
    // Platform buckets. Long-lived: no generation stamp, because nothing wipes
    // this per frame.
    std::unordered_map<CellKey, std::vector<int>, CellKeyHash> staticCells;
    uint32_t staticEpoch = 0;   // platform epoch staticCells was built from; 0 = never
    uint32_t generation    = 0; // bumped once per Rebuild; 0 is never a live value
    uint32_t lastEvictGen  = 0;
};

// Eviction tuning. Sweep every EVICT_INTERVAL frames, dropping cells untouched
// for EVICT_AGE frames (both ~10 s at 60 Hz).
const uint32_t GRID_EVICT_INTERVAL = 600;
const uint32_t GRID_EVICT_AGE      = 600;

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
//
// `outRebuildMs`, if given, receives the time the grid rebuild alone took. A4
// needs the rebuild broken out from the rest of the collision work to size the
// match cap (docs/matchmaking-plan.md); the server passes a pointer, the client
// passes nothing and pays only a null check.
void RunCollisionChecks(GameSpace& space, CollisionGrid& grid,
                        double* outRebuildMs = nullptr);



