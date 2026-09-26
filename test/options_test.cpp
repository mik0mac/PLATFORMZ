// options_test - generateRoomDescription: what a CUSTOM room's one-line
// description says about the rules its host dialed.
//
// An official room's sentence is written by hand in the preset table and needs
// no test. A custom room's is GENERATED, and the two things that can go quietly
// wrong with a generator are the two things checked here: that it reports a rule
// nobody changed (a room playing the defaults should say nothing but where it is
// played), and that it silently stops reporting one that was.
//
//   g++ -std=c++17 -I../server -I/opt/homebrew/include test/options_test.cpp
//
// -I../server is the headless raylib stub: constants.h wants Color, and this
// test wants no graphics stack.
#include "raylib.h"
#include "../options.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what, const std::string& got) {
    printf("  %s %s\n", ok ? "ok:  " : "FAIL:", what.c_str());
    if (!ok) { printf("        got: \"%s\"\n", got.c_str()); failures++; }
}

// `got` says exactly `want`.
static void eq(const std::string& got, const std::string& want, const char* what) {
    check(got == want, std::string(what) + "  ->  \"" + want + "\"", got);
}

// `got` mentions `needle` somewhere.
static void has(const std::string& got, const std::string& needle, const char* what) {
    check(got.find(needle) != std::string::npos,
          std::string(what) + "  ->  says \"" + needle + "\"", got);
}

static void hasnt(const std::string& got, const std::string& needle, const char* what) {
    check(got.find(needle) == std::string::npos,
          std::string(what) + "  ->  does NOT say \"" + needle + "\"", got);
}

int main() {
    printf("a room nobody retuned says only where it is played\n");
    {
        MatchOptions o;
        eq(generateRoomDescription(o), "MEDIUM MAP.", "stock options");
        o.mapSize = "XL";
        eq(generateRoomDescription(o), "XL MAP.", "stock options on another arena");
    }

    printf("\neach rule a host can change earns its own clause\n");
    // One at a time, from stock, so nothing can hide behind the clause cap. A
    // rule that stops being reportable shows up here and nowhere else - the
    // description is cosmetic, so no other test would ever fail for it.
    {
        struct Case { const char* what; void (*tune)(MatchOptions&); const char* says; };
        const Case cases[] = {
            {"walls off",        [](MatchOptions& o){ o.wallsEnabled = false; },            "NO WALLS."},
            {"bouncy walls",     [](MatchOptions& o){ o.wallElasticity = 1.0f; },           "BOUNCY WALLS."},
            {"dead walls",       [](MatchOptions& o){ o.wallElasticity = 0.0f; },           "DEAD WALLS."},
            {"humans only",      [](MatchOptions& o){ o.maxBots = 0; },                     "NO BOTS."},
            {"hard bots",        [](MatchOptions& o){ o.botDifficulty = 0.7f; },            "TOUGH BOTS."},
            {"self-damage off",  [](MatchOptions& o){ o.friendlyFire = false; },            "NO SELF-DAMAGE."},
            {"bigger blasts",    [](MatchOptions& o){ o.explosionRadiusScale = 4.0f; },     "HUGE BLASTS."},
            {"smaller blasts",   [](MatchOptions& o){ o.explosionRadiusScale = 1.0f; },     "SMALL BLASTS."},
            {"faster rockets",   [](MatchOptions& o){ o.rocketSpeedScale = 2.0f; },         "FAST ROCKETS."},
            {"straight rockets", [](MatchOptions& o){ o.rocketsObeyPhysics = false; },      "ROCKETS FLY STRAIGHT."},
            {"speed boost",      [](MatchOptions& o){ o.speedBoost = 1.5f; },               "SPEED BOOSTED."},
            {"jetpack thrust",   [](MatchOptions& o){ o.jetpackThrust = 1.5f; },            "STRONG JETPACK."},
            {"thirsty jetpack",  [](MatchOptions& o){ o.fuelConsumption = 50; },            "FUEL IS SCARCE."},
            {"poor regen",       [](MatchOptions& o){ o.fuelRegenPct = 10; },               "FUEL IS SCARCE."},
            {"generous regen",   [](MatchOptions& o){ o.fuelRegenPct = 90; },               "FUEL IS PLENTIFUL."},
            {"friction on",      [](MatchOptions& o){ o.coastMode = false; },               "NO COAST MODE."},
            {"bouncy platforms", [](MatchOptions& o){ o.platformElasticity = 1.0f; },       "BOUNCY PLATFORMS."},
        };
        for (const Case& c : cases) {
            MatchOptions o;
            c.tune(o);
            has(generateRoomDescription(o), c.says, c.what);
        }
    }

    printf("\na control nudged and put back is not a rule change\n");
    {
        MatchOptions o;
        o.speedBoost     = 1.0f + ROOM_DESC_TOLERANCE * 0.5f;
        o.jetpackThrust  = 1.0f + ROOM_DESC_TOLERANCE * 0.5f;
        eq(generateRoomDescription(o), "MEDIUM MAP.", "both inside the tolerance");
    }

    printf("\nthe line stays short however wild the room\n");
    {
        // Everything at once. The cap is what keeps this on one unwrapped,
        // centred line in the lobby.
        MatchOptions o;
        o.mapSize = "SMALL";
        o.wallsEnabled = false;
        o.maxBots = 0;
        o.friendlyFire = false;
        o.explosionRadiusScale = 4.0f;
        o.rocketSpeedScale = 2.0f;
        o.rocketsObeyPhysics = false;
        o.speedBoost = 2.0f;
        o.jetpackThrust = 2.0f;
        o.fuelConsumption = 90;
        o.coastMode = false;
        o.platformElasticity = 1.0f;
        const std::string got = generateRoomDescription(o);
        // The arena, then exactly ROOM_DESC_MAX_CLAUSES clauses. Counted by
        // full stops, which every clause ends in and none contains - the arena
        // contributes the first one.
        int stops = 0;
        for (char ch : got) if (ch == '.') stops++;
        check(stops == 1 + ROOM_DESC_MAX_CLAUSES,
              "every rule changed at once still fits the clause budget", got);
        has(got, "SMALL MAP.", "leads with the arena");
        has(got, "NO WALLS.", "and keeps the loudest rule");
        hasnt(got, "BOUNCY PLATFORMS.", "while the quietest one is dropped");
    }

    printf("\nwalls off makes their bounce moot rather than contradictory\n");
    {
        MatchOptions o;
        o.wallsEnabled   = false;
        o.wallElasticity = 1.0f;
        has(generateRoomDescription(o), "NO WALLS.", "no walls");
        hasnt(generateRoomDescription(o), "BOUNCY WALLS.", "no walls");
    }

    printf(failures ? "\nsome description checks FAILED\n"
                    : "\nall room-description checks passed\n");
    return failures ? 1 : 0;
}
