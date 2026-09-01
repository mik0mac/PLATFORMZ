// Does the static platform layer (#99) return EXACTLY what the old per-tick
// bucketing returned?
//
// probe.py cannot catch a regression here: a stale or wrong platform bucket shows
// up as "rockets occasionally pass through that one platform", which no phase
// transcript would ever reveal. So compare GatherPlatformNeighbors against a
// brute-force reference for a grid of sample positions across the arena.
//
//   g++ -std=c++17 -O2 -I server -I . -DPLATFORMZ_SERVER \
//       server/test/grid_equiv_test.cpp collisions.cpp -o /tmp/ge && /tmp/ge
#include "gamespace.h"
#include "collisions.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include <tuple>

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL %s\n", what); failures++; }
}

// What the old code produced: every platform whose AABB overlaps any of the 27
// cells around `pos`. Computed here from first principles, independent of the
// grid's own bookkeeping.
static std::vector<int> reference(GameSpace& space, Vector3 pos, float cellSize) {
    auto key = [&](Vector3 p) {
        return CellKey{(int)floorf(p.x / cellSize), (int)floorf(p.y / cellSize),
                       (int)floorf(p.z / cellSize)};
    };
    std::set<std::tuple<int,int,int>> hood;
    CellKey c = key(pos);
    for (int dx=-1; dx<=1; dx++) for (int dy=-1; dy<=1; dy++) for (int dz=-1; dz<=1; dz++)
        hood.insert({c.x+dx, c.y+dy, c.z+dz});

    std::vector<int> out;
    auto& plats = space.getPlatforms();
    for (int i = 0; i < (int)plats.size(); i++) {
        Vector3 half = Vector3Scale(plats[i].size, 0.5f);
        CellKey lo = key(Vector3Subtract(plats[i].position, half));
        CellKey hi = key(Vector3Add(plats[i].position, half));
        bool hit = false;
        for (int cx=lo.x; cx<=hi.x && !hit; cx++)
          for (int cy=lo.y; cy<=hi.y && !hit; cy++)
            for (int cz=lo.z; cz<=hi.z && !hit; cz++)
              if (hood.count({cx,cy,cz})) hit = true;
        if (hit) out.push_back(i);
    }
    std::sort(out.begin(), out.end());
    return out;
}

static void sweep(const char* label, float half, int plats, int roids) {
    GameSpace space;
    space.configureMap(half, plats, roids);
    space.generatePlatforms(); space.spawnPlayers(); space.generateAsteroids();
    CollisionGrid grid;
    grid.Rebuild(space);

    int probes = 0, mismatches = 0;
    const float step = half / 6.0f;
    for (float x = -half; x <= half; x += step)
      for (float y = -half; y <= half; y += step)
        for (float z = -half; z <= half; z += step) {
            std::vector<int> got;
            grid.GatherPlatformNeighbors({x, y, z}, got);
            std::vector<int> want = reference(space, {x, y, z}, 8.0f);
            probes++;
            if (got != want) mismatches++;
        }
    printf("%-7s %5d probe points, %d mismatches\n", label, probes, mismatches);
    check(mismatches == 0, "platform neighbours match the reference");
}

int main() {
    printf("equivalence vs brute force\n");
    sweep("MEDIUM", 120.0f, 128, 18);
    sweep("LARGE",  240.0f, 256, 24);
    sweep("XL",     360.0f, 576, 36);

    printf("invalidation\n");
    {
        GameSpace space;
        space.configureMap(120.0f, 128, 18);
        space.generatePlatforms(); space.spawnPlayers(); space.generateAsteroids();
        CollisionGrid grid;
        grid.Rebuild(space);
        std::vector<int> before;
        grid.GatherPlatformNeighbors({0,0,0}, before);

        // A fresh layout must be picked up - this is the match-restart path.
        space.generatePlatforms();
        grid.Rebuild(space);
        std::vector<int> after;
        grid.GatherPlatformNeighbors({0,0,0}, after);
        check(after == reference(space, {0,0,0}, 8.0f), "regenerate is picked up");

        // A moving platform bumps the epoch every tick, degrading safely to the
        // old per-tick behaviour instead of serving a stale bucket.
        auto& plats = space.getPlatforms();
        plats[0].isMoving = true;
        uint32_t e0 = space.getPlatformEpoch();
        space.updatePositions(1.0f / 60.0f);
        check(space.getPlatformEpoch() != e0, "a moving platform invalidates the layer");
    }

    printf("\n%s\n", failures ? "FAILURES" : "all grid equivalence checks passed");
    return failures ? 1 : 0;
}
