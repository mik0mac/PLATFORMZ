// Does a bot get onto a firing solution, and does accuracy decide how fast?
//
// Bot aim has no random error: accuracy sets turn speed (BOT_TURN_RATE_MIN/MAX),
// and the bot fires only once its current aim would hit (onTarget). So what can
// regress quietly is the TIME to a shot - a steer toward the wrong point, a turn
// rate that stopped scaling, or a dead-centre aim read as a miss (the old
// onTarget did that). None of it shows in a probe transcript; this times it
// against a crossing target. Misses come from the rocket's spread
// (BOT_FIRE_SPREAD_MIN/MAX), checked at the bottom.
//
//   g++ -std=c++17 -O2 -I server -I . -DPLATFORMZ_SERVER \
//       server/test/bot_aim_test.cpp collisions.cpp -o /tmp/ba && /tmp/ba
#include "bot_logic.h"
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok:  " : "FAIL:", what);
    if (!ok) failures++;
}

static const float DT = 1.0f / 60.0f;

// Seconds until a bot facing `startYaw` (target straight down +x) first has a
// firing solution on a target crossing at 20 u/s, 60 units out. -1 if never
// within 5 s.
static float TimeToSolution(float accuracy, float startYaw, float rocketSpeed = ROCKET_SPEED) {
    Player bot;    bot.position = {0, 0, 0}; bot.yaw = startYaw; bot.pitch = 0.0f;
    Player target; target.position = {60, 10, 0}; target.velocity = {0, 0, 20};
    std::vector<Player> players{bot, target};
    std::vector<Platform> platforms; std::vector<Asteroid> asteroids;
    Walls walls; std::vector<BotDecision> decisions; // steerAimToward reads no decision slots
    BotProfile profile; profile.accuracy = accuracy;

    for (float t = 0.0f; t < 5.0f; t += DT) {
        PlayerInput out{};
        Blackboard<Player> bb{bot, target, players, platforms, asteroids, walls, out, DT,
                              decisions, profile, rocketSpeed, EXPLOSION_DAMAGE_RADIUS,
                              true, 0.0f, 0.0f};
        auto [hit, aimDir] = onTarget(target, bot, rocketSpeed);
        if (hit) return t;
        steerAimToward(bb, aimDir);
        bot.updateLook(out.lookDelta);
        target.position = Vector3Add(target.position, Vector3Scale(target.velocity, DT));
    }
    return -1.0f;
}

int main() {
    // The intercept is a real meeting point: a rocket fired along it reaches the
    // target's future position.
    {
        Player shooter; shooter.position = {0, 0, 0};
        Player target;  target.position = {60, 10, 0}; target.velocity = {0, 0, 20};
        Vector3 dir = interceptDirection(target, shooter, ROCKET_SPEED);
        float best = 1e9f;
        for (float t = 0.0f; t < 1.0f; t += 0.0005f) {
            Vector3 rocket = Vector3Scale(dir, ROCKET_SPEED * t);
            Vector3 tgt = Vector3Add(target.position, Vector3Scale(target.velocity, t));
            best = std::min(best, Vector3Distance(rocket, tgt));
        }
        check(best < 0.2f, "interceptDirection leads a crossing target to a meeting point");
    }

    // Aim exactly down the intercept counts as on target (the old onTarget read
    // a dead-centre aim as a miss).
    {
        Player shooter; shooter.position = {0, 0, 0};
        Player target;  target.position = {60, 0, 0}; target.velocity = {0, 0, 0};
        shooter.yaw = 0.0f; shooter.pitch = 0.0f; // Forward() == +x, dead centre
        auto [hit, dir] = onTarget(target, shooter, ROCKET_SPEED);
        check(hit, "dead-centre aim is on target");
    }

    // Time to a firing solution, from 90 degrees off.
    float slow = TimeToSolution(0.0f, PI / 2), mid = TimeToSolution(0.5f, PI / 2),
          fast = TimeToSolution(1.0f, PI / 2);
    printf("  time to solution from 90deg: acc 0 %.2fs, acc 0.5 %.2fs, acc 1 %.2fs\n", slow, mid, fast);
    check(slow > 0 && mid > 0 && fast > 0, "every accuracy finds a solution");
    check(fast < mid && mid < slow, "higher accuracy finds it sooner");
    // Turning 90deg at MAX takes (PI/2)/MAX; leave a little for the lead.
    check(fast < (PI / 2) / BOT_TURN_RATE_MAX + 0.15f, "accuracy 1 is on target about as fast as it can turn");

    // Fast rockets (CLASSIC HYPED: 2x rocket speed * 1.5x speed boost) need less lead.
    float hyped = TimeToSolution(1.0f, PI / 2, ROCKET_SPEED * 3.0f);
    check(hyped > 0 && hyped < (PI / 2) / BOT_TURN_RATE_MAX + 0.15f, "...and with HYPED rocket speed");

    // Rocket spread: every direction inside the cone, and the cone actually used.
    {
        Vector3 aim = Vector3Normalize({1, 0.3f, -0.4f});
        float spread = 0.1f, widest = 0.0f;
        bool inside = true;
        for (int i = 0; i < 20000; i++) {
            float a = acosf(Clamp(Vector3DotProduct(aim, SpreadDirection(aim, spread)), -1.0f, 1.0f));
            if (a > spread + 1e-3f) inside = false;
            widest = std::max(widest, a);
        }
        check(inside, "every spread shot stays inside the cone");
        check(widest > 0.95f * spread, "...and reaches its edge");
        Vector3 up = {0, 1, 0};
        check(Vector3Distance(SpreadDirection(up, 0.0f), up) < 1e-6f, "zero spread fires straight (humans)");
        check(fabsf(Vector3Length(SpreadDirection(up, 0.1f)) - 1.0f) < 1e-4f, "straight-up aim still gives a unit direction");
    }

    // FireAtPlayer hands the spread over by accuracy; humans' default stays 0.
    {
        check(PlayerInput{}.fireSpread == 0.0f, "a human's PlayerInput has no spread");
        auto spreadAt = [](float accuracy) {
            Player bot;    bot.position = {0, 0, 0}; bot.yaw = 0.0f; bot.pitch = 0.0f;
            Player target; target.position = {60, 0, 0};
            std::vector<Player> players{bot, target};
            std::vector<Platform> platforms; std::vector<Asteroid> asteroids;
            Walls walls; std::vector<BotDecision> decisions;
            BotProfile profile; profile.accuracy = accuracy;
            PlayerInput out{};
            Blackboard<Player> bb{bot, target, players, platforms, asteroids, walls, out, DT,
                                  decisions, profile, ROCKET_SPEED, EXPLOSION_DAMAGE_RADIUS,
                                  true, 0.0f, 0.0f};
            FireAtPlayer<Player> node;
            node.tick(bb);
            return out.fire ? out.fireSpread : -1.0f;
        };
        check(fabsf(spreadAt(0.0f) - BOT_FIRE_SPREAD_MAX) < 1e-6f, "accuracy 0 fires with BOT_FIRE_SPREAD_MAX");
        check(fabsf(spreadAt(1.0f) - BOT_FIRE_SPREAD_MIN) < 1e-6f, "accuracy 1 fires with BOT_FIRE_SPREAD_MIN");
    }

    if (failures) { printf("\n%d bot-aim check(s) FAILED\n", failures); return 1; }
    printf("\nall bot-aim checks passed\n");
    return 0;
}
