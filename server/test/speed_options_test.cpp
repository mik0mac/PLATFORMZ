// Does each OPTIONS speed slider move only its own thing?
//
// SPEED BOOST is the walk, JETPACK THRUST the jetpack, ROCKET VELOCITY the
// rockets - one each, never stacked. SPEED BOOST used to multiply all three, so
// what this guards against is it quietly leaking back into the jetpack or a
// rocket. Every case drives the real ApplyPlayerInput path (bots and the server
// sim use the same one) and compares a boosted run against a 1x run.
//
//   g++ -std=c++17 -O2 -I server -I . -DPLATFORMZ_SERVER \
//       server/test/speed_options_test.cpp collisions.cpp -o /tmp/so && /tmp/so
#include "gamespace.h"
#include "input.h"
#include <cmath>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok:  " : "FAIL:", what);
    if (!ok) failures++;
}
static bool near(float a, float b, float eps = 0.01f) { return fabsf(a - b) <= eps; }

static const float DT = 1.0f / 60.0f;

struct Speeds { float horizontal, vertical; };

// Hold `in` for 3 s from rest, floating (no gravity, so vertical is the
// jetpack's own target), with a full tank kept topped up. Returns the settled
// velocity.
static Speeds Settle(float boost, float thrust, PlayerInput in) {
    GameSpace gs;
    gs.speedBoost    = boost;
    gs.jetpackThrust = thrust;
    gs.coastMode     = false;               // friction: speed settles at the target
    Player p;
    p.yaw = 0.0f; p.pitch = 0.0f;
    for (float t = 0.0f; t < 3.0f; t += DT) {
        p.fuel = 100.0f;
        ApplyPlayerInput(p, in, DT, 0.0f, gs);
    }
    return { sqrtf(p.velocity.x * p.velocity.x + p.velocity.z * p.velocity.z), p.velocity.y };
}

static float RocketSpeed(float boost, float rspeed) {
    GameSpace gs;
    gs.speedBoost       = boost;
    gs.rocketSpeedScale = rspeed;
    gs.rocketsObeyPhysics = false;          // no inherited velocity
    Player p;
    PlayerInput in{};
    in.fire = true;
    ApplyPlayerInput(p, in, DT, 0.0f, gs);
    if (gs.getRockets().empty()) return -1.0f;
    return gs.getRockets().back().speed;
}

int main() {
    PlayerInput walk{};  walk.moveAxis = {0, 1};
    PlayerInput hover{}; hover.jetpack = true;
    PlayerInput fly{};   fly.jetpack = true; fly.moveAxis = {0, 1};

    printf("walking\n");
    Speeds w1 = Settle(1.0f, 1.0f, walk), w2 = Settle(2.0f, 1.0f, walk);
    check(near(w1.horizontal, PLAYER_SPEED_WALK), "1x walks at PLAYER_SPEED_WALK");
    check(near(w2.horizontal, 2.0f * PLAYER_SPEED_WALK), "SPEED BOOST 2x doubles the walk");
    check(near(Settle(1.0f, 2.0f, walk).horizontal, PLAYER_SPEED_WALK), "JETPACK THRUST leaves the walk alone");

    printf("jetpack, straight up\n");
    Speeds h1 = Settle(1.0f, 1.0f, hover), hB = Settle(2.0f, 1.0f, hover), hT = Settle(1.0f, 1.5f, hover);
    check(near(h1.vertical, PLAYER_SPEED_JETPACK), "1x climbs at PLAYER_SPEED_JETPACK");
    check(near(hB.vertical, h1.vertical), "SPEED BOOST does not change the climb");
    check(near(hT.vertical, 1.5f * PLAYER_SPEED_JETPACK), "JETPACK THRUST 1.5x scales the climb");

    printf("jetpack, flying forward\n");
    Speeds f1 = Settle(1.0f, 1.0f, fly), fB = Settle(1.5f, 1.0f, fly), fT = Settle(1.0f, 2.0f, fly);
    check(near(f1.horizontal, PLAYER_SPEED_JETPACK), "1x flies at PLAYER_SPEED_JETPACK");
    check(near(fB.horizontal, f1.horizontal) && near(fB.vertical, f1.vertical),
          "SPEED BOOST 1.5x (walk still under the jetpack) leaves flight alone");
    check(near(fT.horizontal, 2.0f * PLAYER_SPEED_JETPACK), "JETPACK THRUST 2x scales flight");
    Speeds fBig = Settle(2.0f, 1.0f, fly);
    check(near(fBig.horizontal, 2.0f * PLAYER_SPEED_WALK),
          "a walk boosted past the jetpack: holding Space never flies slower than walking");
    check(near(fBig.vertical, f1.vertical), "...and the climb is still unboosted");

    printf("rockets\n");
    float r1 = RocketSpeed(1.0f, 1.0f);
    check(near(r1, ROCKET_SPEED), "1x fires at ROCKET_SPEED");
    check(near(RocketSpeed(2.0f, 1.0f), r1), "SPEED BOOST does not change rocket speed");
    check(near(RocketSpeed(2.0f, 2.0f), 2.0f * ROCKET_SPEED), "ROCKET VELOCITY alone scales it");

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("speed options: all passed\n");
    return 0;
}
