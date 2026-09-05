// Where does CollisionGrid::Rebuild spend its time?
//
// A4 found the rebuild is still ~80% of a tick even after A1b removed its
// allocation churn. This splits the cost by rebuilding the same world twice: once
// as generated, and once with the platforms removed. The gap is what platforms
// cost - and platforms never move after generatePlatforms(), so that whole gap is
// work being repeated 60 times a second for no reason.
//
//   g++ -std=c++17 -O2 -I server -I . -DPLATFORMZ_SERVER \
//       server/test/bench_grid_split.cpp collisions.cpp -o /tmp/split && /tmp/split
//
// Absolute numbers here run far below the live server's p95 because this bench
// runs alone; the RATIO is the finding. See docs/perf-measurements.md.
#include <cstdio>
#include <chrono>
#include "gamespace.h"
#include "collisions.h"

static double timeRebuilds(GameSpace& space, int n) {
    CollisionGrid g;
    g.Rebuild(space);                       // warm
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) g.Rebuild(space);
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count() / n;
}

static void run(const char* label, float half, int plats, int roids) {
    GameSpace full;
    full.configureMap(half, plats, roids);
    full.generatePlatforms(); full.spawnPlayers(); full.generateAsteroids();

    // same world with the platforms removed, to isolate their share
    GameSpace dyn;
    dyn.configureMap(half, 0, roids);
    dyn.generatePlatforms(); dyn.spawnPlayers(); dyn.generateAsteroids();

    double all = timeRebuilds(full, 400);
    double nop = timeRebuilds(dyn, 400);
    printf("%-7s platforms=%-4d | rebuild %.3f ms | dynamic-only %.3f ms | "
           "platforms are %.0f%% of it\n",
           label, plats, all, nop, 100.0 * (all - nop) / all);
}

int main() {
    run("MEDIUM", 120.0f, 128, 18);
    run("LARGE",  240.0f, 256, 24);
    run("XL",     360.0f, 576, 36);
    return 0;
}
