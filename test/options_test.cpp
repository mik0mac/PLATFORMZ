// options_test - generateRoomDescription: what a CUSTOM room's one-line
// description says about the rules its host dialed.
//
// An official room's sentence is written by hand in the preset table and needs
// no test. A custom room's is GENERATED, and the two things that can go quietly
// wrong with a generator are the two things checked here: that it reports a rule
// nobody changed (a room playing the defaults should say nothing but where it is
// played), and that it silently stops reporting one that was.
//
// THE REPORTED SET IS A SUBSET ON PURPOSE. Most of the fifteen rules are dialed
// without changing how a room FEELS to walk into, so options.h reports a chosen
// few and leaves the rest to the OPTIONS modal, which shows every value anyway.
// Both halves are pinned below - the ones that speak and the ones that stay
// quiet - so re-enabling a clause is one line moved from the second table to the
// first, and dropping one by accident is a failure rather than a shrug.
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

static void eq(const std::string& got, const std::string& want, const char* what) {
    check(got == want, std::string(what) + "  ->  \"" + want + "\"", got);
}

static void has(const std::string& got, const std::string& needle, const char* what) {
    check(got.find(needle) != std::string::npos,
          std::string(what) + "  ->  says \"" + needle + "\"", got);
}

static void hasnt(const std::string& got, const std::string& needle, const char* what) {
    check(got.find(needle) == std::string::npos,
          std::string(what) + "  ->  does NOT say \"" + needle + "\"", got);
}

// Sentences, counted by their terminators - every clause ends in one and none
// contains one. Not just '.', because a clause is allowed to shout.
static int sentences(const std::string& s) {
    int n = 0;
    for (char c : s) if (c == '.' || c == '!') n++;
    return n;
}

struct Case { const char* what; void (*tune)(MatchOptions&); const char* says; };

int main() {
    printf("a room nobody retuned says only where it is played\n");
    {
        MatchOptions o;
        eq(generateRoomDescription(o), "MEDIUM map.", "stock options");
        o.mapSize = "XL";
        eq(generateRoomDescription(o), "XL map.", "stock options on another arena");
    }

    printf("\nthe rules that speak\n");
    // One at a time, from stock, so nothing can hide behind the clause cap. A
    // rule that stops being reportable shows up here and nowhere else - the
    // description is cosmetic, so no other test would ever fail for it.
    {
        const Case cases[] = {
            {"walls off",        [](MatchOptions& o){ o.wallsEnabled = false; },       "No walls."},
            {"humans only",      [](MatchOptions& o){ o.maxBots = 0; },                "No bots."},
            {"self-damage off",  [](MatchOptions& o){ o.friendlyFire = false; },       "No self-damage."},
            {"straight rockets", [](MatchOptions& o){ o.rocketsObeyPhysics = false; }, "Rocket physics off."},
            {"speed boost",      [](MatchOptions& o){ o.speedBoost = 1.5f; },          "Speed boosted!"},
            {"friction on",      [](MatchOptions& o){ o.coastMode = false; },          "No coast."},
            // Scarcity is an absolute judgement, not a comparison with the
            // defaults: a thirsty jetpack AND a regen that cannot keep up.
            {"thirsty jetpack",  [](MatchOptions& o){ o.fuelConsumption = 60;
                                                      o.fuelRegenPct = 40; },          "Fuel is scarce."},
        };
        for (const Case& c : cases) {
            MatchOptions o;
            c.tune(o);
            has(generateRoomDescription(o), c.says, c.what);
        }
    }

    printf("\nthe rules that stay quiet, deliberately\n");
    // Dialable, and changed here, but not worth a word: the line is one
    // unwrapped row and these do not change what walking into the room is like.
    // To start reporting one, move it up to the table above.
    {
        const Case quiet[] = {
            {"bouncy walls",     [](MatchOptions& o){ o.wallElasticity = 1.0f; },       nullptr},
            {"dead walls",       [](MatchOptions& o){ o.wallElasticity = 0.0f; },       nullptr},
            {"hard bots",        [](MatchOptions& o){ o.botDifficulty = 0.7f; },        nullptr},
            {"bigger blasts",    [](MatchOptions& o){ o.explosionRadiusScale = 4.0f; }, nullptr},
            {"faster rockets",   [](MatchOptions& o){ o.rocketSpeedScale = 2.0f; },     nullptr},
            {"jetpack thrust",   [](MatchOptions& o){ o.jetpackThrust = 2.0f; },        nullptr},
            {"bouncy platforms", [](MatchOptions& o){ o.platformElasticity = 1.0f; },   nullptr},
            // Generous fuel is not news either - only scarce fuel is.
            {"generous regen",   [](MatchOptions& o){ o.fuelRegenPct = 90; },           nullptr},
            // Half of the scarcity test on its own is not scarcity.
            {"thirsty but fed",  [](MatchOptions& o){ o.fuelConsumption = 60;
                                                      o.fuelRegenPct = 60; },          nullptr},
        };
        for (const Case& c : quiet) {
            MatchOptions o;
            c.tune(o);
            eq(generateRoomDescription(o), "MEDIUM map.", c.what);
        }
    }

    printf("\na control nudged and put back is not a rule change\n");
    {
        MatchOptions o;
        o.speedBoost = 1.0f + ROOM_DESC_TOLERANCE * 0.5f;
        eq(generateRoomDescription(o), "MEDIUM map.", "inside the tolerance");
        o.speedBoost = 1.0f + ROOM_DESC_TOLERANCE * 2.0f;
        has(generateRoomDescription(o), "Speed boosted!", "outside it");
    }

    printf("\nthe line stays short however wild the room\n");
    {
        // Everything reportable at once. The cap is what keeps this on one
        // unwrapped, centred line in the lobby.
        MatchOptions o;
        o.mapSize = "SMALL";
        o.wallsEnabled = false;
        o.friendlyFire = false;
        o.rocketsObeyPhysics = false;
        o.speedBoost = 2.0f;
        o.coastMode = false;
        o.fuelConsumption = 90;
        o.fuelRegenPct = 5;
        const std::string got = generateRoomDescription(o);
        check(sentences(got) == 1 + ROOM_DESC_MAX_CLAUSES,
              "the arena plus exactly ROOM_DESC_MAX_CLAUSES clauses", got);
        has(got, "SMALL map.", "leads with the arena");
        has(got, "No walls.", "and keeps the loudest rule");
        hasnt(got, "No coast.", "while the quietest one is dropped");
    }

    printf("\n...but NO BOTS is said whether or not the budget is spent\n");
    {
        // It is appended rather than queued, because "there are no bots here" is
        // not a flourish - it is the single fact most likely to change whether
        // somebody wants the room at all.
        MatchOptions o;
        o.mapSize = "SMALL";
        o.wallsEnabled = false;
        o.friendlyFire = false;
        o.rocketsObeyPhysics = false;
        o.speedBoost = 2.0f;
        o.coastMode = false;
        o.maxBots = 0;
        const std::string got = generateRoomDescription(o);
        has(got, "No bots.", "budget full");
        check(sentences(got) == 2 + ROOM_DESC_MAX_CLAUSES,
              "and it costs a sentence the cap does not count", got);
    }

    printf(failures ? "\nsome description checks FAILED\n"
                    : "\nall room-description checks passed\n");
    return failures ? 1 : 0;
}
