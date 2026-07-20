#pragma once

// MARK: BotController — shared bot orchestration
// -----------------------------------------------------------------------------
// The behaviour-tree NODE TYPES live in bot_logic.h and are already reusable;
// this header owns the parts that used to sit inline in main.cpp: the concrete
// tree instance, the per-slot decision/personality state, and the spawn/drive
// glue. Both the local client (main.cpp) and the authoritative server
// (server/server_main.cpp) construct a BotController so the two build byte-for-
// byte identical bots from a SINGLE source of truth.
//
// The tree stores raw pointers between its member nodes, so a BotController must
// not be copied or moved after construction — keep it as a stable local/global
// that outlives the match.

#include "bot_logic.h"   // Node types, botInput(), BotProfile, BotDecision, Blackboard
#include "gamespace.h"   // GameSpace, Player
#include "input.h"       // PlayerInput, ApplyPlayerInput
#include "constants.h"   // BOT_DIFFICULTY, BOT_PERSONALITY_SPREAD, MOON/EARTH_GRAVITY

#include <vector>
#include <random>
#include <cfloat>

struct BotController {
    // One latch slot per LatchedSelector in the tree. Each bot gets a
    // BotDecision vector sized to LATCH_COUNT so nested/sibling latches keep
    // independent state (see LatchedSelector). Add a new LatchedSelector? Give
    // it an id here.
    enum LatchId { LATCH_MOVEMENT, LATCH_MOVE_TO_SAFETY,
                   LATCH_ATTACK_STYLE, LATCH_FIRE,
                   LATCH_KITE_CHANCE, LATCH_RETREAT_CD,
                   LATCH_FIRE_PLAYER_CD, LATCH_ATTACK_AST_CD,
                   LATCH_AVOID_WALL, LATCH_DEFEND_CHANCE, LATCH_COUNT };

    // --- Leaf nodes ---
    IsLowFuel<Player>      isLowFuel;
    IsLowHealth<Player>    isLowHealth;
    NeedsBonus<Player>     needsBonus;
    FindLineOfSight<Player> findLineOfSight;
    FindCover<Player>      findCover;
    MoveToPlayer<Player>   moveToPlayer;
    MoveFromPlayer<Player> moveFromPlayer;
    MoveToPlatform<Player> moveToPlatform;
    FireAtPlayer<Player>   fireAtPlayer;
    AttackAsteroid<Player> attackAsteroid;
    AvoidAsteroid<Player>  avoidAsteroid;
    AvoidWall<Player>      avoidWall{ LATCH_AVOID_WALL };
    Idle<Player>           idle;
    SeekHighGround<Player>   seekHighGround;
    Bounce<Player>          bounce;

    // --- Composed tree (declaration order matters: composites reference the
    // addresses of nodes declared above; members initialise top-to-bottom). ---



    // Rate-limit the bots' fire to make the difficulty setting meaningful. Fire
    // cadence scales with aggression: aggressive bots approach PLAYER_FIRE_RATE,
    // timid bots fire ~1/4 as often. Computed per-bot from bb.profile because the
    // tree nodes are shared across all bots (there's no bot instance at construction).
    static float botFirePeriod(Blackboard<Player>& bb) {
        float rate = (BOT_FIRERATE_MAX - BOT_FIRERATE_MIN) * bb.profile.aggression + BOT_FIRERATE_MIN; // shots/sec
        return 1.0f / rate; // seconds between shots (the Cooldown period)
    }
    Cooldown<Player>               rateLimitedFireAtPlayer{ LATCH_FIRE_PLAYER_CD, botFirePeriod, &fireAtPlayer };
    Cooldown<Player>               rateLimitedAttackAsteriod { LATCH_ATTACK_AST_CD, botFirePeriod, &attackAsteroid };
    // A more aggressive bot has a high chance of ignoring the low-health retreat
    // and fighting on. Per-bot because the tree nodes are shared across all bots.
    static float defenseProbability(Blackboard<Player>& bb) {
        return 1.0f - BOT_AGGRO_SKIP_DEFENSE * bb.profile.aggression; // aggressive bots rarely defend
    }
    // needsBonus is a GUARD, so it gates attackAsteroid via a Sequence: only hunt
    // asteroids when a bonus is actually wanted.
    Sequence<Player>               attackAsteroidForBonus{ { &needsBonus, &rateLimitedAttackAsteriod } };
    // fireAtTarget sits in the top Parallel, so aim/fire stay per-frame regardless
    // of the movement decision cadence.
    LatchedSelector<Player>        fireAtTarget{ LATCH_FIRE, { &rateLimitedFireAtPlayer, &attackAsteroidForBonus } };
    // Plain Selector, not latched: both children's "am I still applicable"
    // check is purely positional (SeekHighGround's platform search, Bounce's
    // distance to the floor) and can flip within a single frame, so latching
    // onto one for ~1s would leave a bot stuck re-running a child that's
    // already done and writing nothing (see Bounce's Failure-on-done comment).
    // Retreat: a hurt bot doesn't ALWAYS flee (Chance gates the kite). "Kiting":
    // retreating while keeping an enemy at a distance you control. avoidAsteroid
    // stays a hard priority ahead of the kite/cover.
    Selector<Player>               conserveFuel{ { &seekHighGround, &bounce } };
    Chance<Player>                 maybeKite{ LATCH_KITE_CHANCE, 0.5f, &moveFromPlayer };
    LatchedSelector<Player>        moveToSafety{ LATCH_MOVE_TO_SAFETY, { &avoidAsteroid, &maybeKite, &findCover } };
    Sequence<Player>               lowHealthResponse{ { &isLowHealth, &moveToSafety, &idle } };
    // Gate the whole low-health retreat by an aggression-scaled roll: aggressive
    // bots usually skip it (maybeDefend -> Failure) and fall through to attack.
    Chance<Player>                 maybeDefend{ LATCH_DEFEND_CHANCE, defenseProbability, &lowHealthResponse };
    Sequence<Player>               lowFuelResponse{ { &isLowFuel, &moveToPlatform, &idle } };
    // Attack: personality-weighted random pick between sniping from range
    // (findLineOfSight) and closing in (moveToPlayer) — aggressive bots close,
    // timid bots hold. avoidAsteroid is a hard priority ahead of either.
    WeightedRandomSelector<Player> attackStyle{ LATCH_ATTACK_STYLE, {
        { &findLineOfSight, [](Blackboard<Player>& bb){ return 1.0f - bb.profile.aggression; } },
        { &moveToPlayer,    [](Blackboard<Player>& bb){ return bb.profile.aggression; } },
    } };
    // avoidWall is a hard priority ahead of the hold/close logic so a bot parked
    // on a boundary peels off (then releases back to attackStyle once clear).
    Selector<Player>               attack{ { &avoidAsteroid, &avoidWall, &attackStyle } };
    LatchedSelector<Player>        movement{ LATCH_MOVEMENT, { &conserveFuel, &maybeDefend, &lowFuelResponse, &attack } };
    Parallel<Player>               botTree{ { &movement, &fireAtTarget } };

    // --- Per-slot state, indexed BY PLAYER INDEX (slot 0's entry is simply
    // unused wherever slot 0 is a human). ---
    std::vector<std::vector<BotDecision>> decisions; // per slot -> one BotDecision per LatchedSelector (LATCH_COUNT slots)
    std::vector<BotProfile>               profiles;  // per slot -> personality (aggression/accuracy)

    // Size the per-slot state to the player count and seed each slot's
    // personality deterministically from its player id, so the same map replays
    // the same bots. Does NOT touch isBot/name/color — the caller owns which
    // slots are bots (local: slots 1+, server: unoccupied slots).
    void init(std::vector<Player>& players, float difficulty = BOT_DIFFICULTY) {
        int n = (int)players.size();
        decisions.assign(n, std::vector<BotDecision>(LATCH_COUNT));
        profiles.assign(n, BotProfile{});
        // Personality spread widens with bot count: a lone bot ~= difficulty,
        // a crowd fans out around it. n-1 potential bots (slot 0 reserved for the
        // human in local mode) matches the previous local-mode spread.
        int nBots = n - 1;
        float spread = nBots > 0 ? BOT_PERSONALITY_SPREAD * (nBots - 1) / nBots : 0.0f;
        for (int i = 0; i < n; ++i) {
            std::mt19937 rng(players[i].id);
            std::uniform_real_distribution<float> jitter(-spread, spread);
            profiles[i].aggression = Clamp(difficulty + jitter(rng), 0.0f, 1.0f);
            profiles[i].accuracy   = Clamp(difficulty + jitter(rng), 0.0f, 1.0f);
        }
    }

    // Tick the behaviour tree once for every slot flagged isBot and apply the
    // resulting input through the same path a human uses. The target is the
    // nearest human (isBot == false); with a single human at slot 0 this is
    // exactly the players[0] target the local drive loop used before.
    void drive(GameSpace& gs, float dt) {
        std::vector<Player>& players = gs.getPlayers();
        // Grow lazily if the controller was never init()'d for this many slots
        // (defensive — callers init() at match start).
        if ((int)decisions.size() < (int)players.size()) init(players);
        // OPTIONS-scaled values so lead-aim/standoff match what actually happens
        // in collisions/input.h: rockets spawn at ROCKET_SPEED * speedBoost *
        // rocketSpeedScale (input.h), blasts detonate at EXPLOSION_DAMAGE_RADIUS *
        // explosionRadiusScale (collisions.cpp spawnExplosion).
        float rocketSpeed = ROCKET_SPEED * gs.speedBoost * gs.rocketSpeedScale;
        float explosionRadius = EXPLOSION_DAMAGE_RADIUS * gs.explosionRadiusScale;
        // Gates for the fuel-conserving / wall-bounce behaviors (bot_logic.h:
        // SeekHighGround, Bounce) - read straight off GameSpace's OPTIONS fields.
        bool  wallsEnabled = gs.wallsEnabled;
        float fuelConsumptionRate = gs.fuelConsumptionRate;
        float fuelRegenRate = gs.fuelRegenRate();
        for (int i = 0; i < (int)players.size(); ++i) {
            if (!players[i].isBot || !players[i].isAlive) continue;
            int targetIdx = pickTarget(players, i);
            if (targetIdx == i) {
                // No living target (bot is last alive): idle, but still run the
                // physics half of the input path. Gravity lives in
                // updateVelocity, so skipping entirely would freeze the bot's
                // velocity and let it drift in a straight line forever.
                ApplyPlayerInput(players[i], PlayerInput{}, dt, MOON_GRAVITY, gs);
                continue;
            }
            PlayerInput botIn = botInput(players[i], players[targetIdx], players,
                                         gs.getPlatforms(), gs.getAsteroids(), gs.getWalls(),
                                         botTree, dt, decisions[i], profiles[i],
                                         rocketSpeed, explosionRadius,
                                         wallsEnabled, fuelConsumptionRate, fuelRegenRate);
            float gravity = botIn.earthGravity ? EARTH_GRAVITY : MOON_GRAVITY;
            ApplyPlayerInput(players[i], botIn, dt, gravity, gs);
        }
    }

private:
    // Nearest living human to slot `self`. Falls back to the nearest other
    // living player, then to self (harmless — only reached if this bot is the
    // only one left alive). Dead players are free-flying spectators; targeting
    // them means shooting at a ghost.
    static int pickTarget(const std::vector<Player>& players, int self) {
        int bestHuman = -1, bestAny = -1;
        float dHuman = FLT_MAX, dAny = FLT_MAX;
        for (int j = 0; j < (int)players.size(); ++j) {
            if (j == self || !players[j].isAlive) continue;
            float d = Vector3DistanceSqr(players[self].position, players[j].position);
            if (d < dAny) { dAny = d; bestAny = j; }
            if (!players[j].isBot && d < dHuman) { dHuman = d; bestHuman = j; }
        }
        if (bestHuman >= 0) return bestHuman;
        if (bestAny   >= 0) return bestAny;
        return self;
    }
};
