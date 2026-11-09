// Does a rocket detonate where CheckRocketPlayerCollisions says it should?
//
// The fuse is swept (prevPosition -> position) and brute-forced over every
// player, so the failures this catches are the quiet ones: a fast rocket
// stepping over a player between ticks, a fuse that detonates at its edge
// instead of the closest approach, or a direct hit that no longer goes off at
// the body. None of those shows up in a probe transcript.
//
//   g++ -std=c++17 -O2 -I server -I . -DPLATFORMZ_SERVER \
//       server/test/rocket_fuse_test.cpp collisions.cpp -o /tmp/rf && /tmp/rf
#include "gamespace.h"
#include "collisions.h"
#include <cmath>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok:  " : "FAIL:", what);
    if (!ok) failures++;
}
static bool near(Vector3 a, Vector3 b, float eps = 0.01f) {
    return Vector3Distance(a, b) <= eps;
}

// One frame of travel p0 -> p1 for a rocket owned by player 99, against a single
// target at the origin (plus any extra players). Returns whether it detonated,
// and where.
struct Result { bool hit; Vector3 at; };
static Result Fire(Vector3 p0, Vector3 p1, std::vector<Vector3> others = {}) {
    GameSpace space;
    auto& players = space.getPlayers();
    players.clear();
    Player target; target.id = 1; target.position = {0, 0, 0};
    players.push_back(target);
    for (size_t i = 0; i < others.size(); i++) {
        Player p; p.id = 2 + (uint32_t)i; p.position = others[i];
        players.push_back(p);
    }
    Player shooter; shooter.id = 99; shooter.position = p0; // sits on the path: must be ignored
    players.push_back(shooter);

    Rocket r; r.ownerId = 99; r.prevPosition = p0; r.position = p1;
    space.getRockets().push_back(r);
    space.getExplosions().clear();

    CollisionGrid grid;
    CheckRocketPlayerCollisions(space, grid);
    auto& ex = space.getExplosions();
    if (ex.empty()) return {false, {}};
    return {true, ex.back().position};
}

int main() {
    const float fuse  = ROCKET_PROXIMITY_RADIUS + ROCKET_RADIUS;
    const float body  = PLAYER_RADIUS + ROCKET_RADIUS;
    printf("fuse %.2f, body %.2f\n", fuse, body);

    // Direct hit: goes off where the path enters the body, as it always did.
    Result r = Fire({-20, 0, 0}, {20, 0, 0});
    check(r.hit && near(r.at, {-body, 0, 0}), "direct hit detonates at the body's entry point");

    // A rocket fast enough to jump the whole fuse in one tick still hits.
    r = Fire({-40, 0, 0}, {40, 0, 0});
    check(r.hit && near(r.at, {-body, 0, 0}), "an 80-unit step cannot tunnel through a direct hit");
    r = Fire({-40, fuse - 0.5f, 0}, {40, fuse - 0.5f, 0});
    check(r.hit && near(r.at, {0, fuse - 0.5f, 0}), "an 80-unit near-miss detonates at closest approach");

    // Near-miss inside the fuse: at the closest point, not the fuse's edge.
    float off = (body + fuse) * 0.5f;
    r = Fire({-20, off, 0}, {20, off, 0});
    check(r.hit && near(r.at, {0, off, 0}), "near-miss detonates at closest approach");

    // Just outside the fuse: nothing.
    r = Fire({-20, fuse + 0.1f, 0}, {20, fuse + 0.1f, 0});
    check(!r.hit, "a pass just outside the fuse does not detonate");

    // Inside the fuse but still closing at frame end: wait for the closest point.
    r = Fire({-20, off, 0}, {-1, off, 0});
    check(!r.hit, "still closing in at frame end: waits a tick");
    r = Fire({-1, off, 0}, {20, off, 0});
    check(r.hit && near(r.at, {0, off, 0}), "next tick detonates at closest approach");

    // The shooter is on the path from the start; it never sets the rocket off.
    r = Fire({0, 30, 0}, {0, 40, 0});
    check(!r.hit, "the rocket's owner never triggers the fuse");

    // Two players in range: the one reached first along the path wins.
    r = Fire({-20, 0, 0}, {40, 0, 0}, {{15, 0, 0}});
    check(r.hit && near(r.at, {-body, 0, 0}), "the first player along the path wins");
    r = Fire({40, 0, 0}, {-20, 0, 0}, {{15, 0, 0}});
    check(r.hit && near(r.at, {15 + body, 0, 0}), "...from either direction");

    // Far beyond any grid cell's reach (8): the fuse still sees it.
    r = Fire({-20, 0, 30}, {20, 0, 30}, {{0, 0, 30 + fuse - 0.5f}});
    check(r.hit && near(r.at, {0, 0, 30}), "no grid: players anywhere are tested");

    if (failures) { printf("\n%d rocket-fuse check(s) FAILED\n", failures); return 1; }
    printf("\nall rocket-fuse checks passed\n");
    return 0;
}
