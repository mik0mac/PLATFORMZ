// MatchOptions: the full set of player-selectable match rules driven by the
// OPTIONS modal (main.cpp). One struct so the modal, startGame, the wire
// (serializeOptions/serializeStart in wire.h), and the server's apply-on-start
// all pass the same bundle instead of an ever-growing positional arg list.
// Defaults come from constants.h so an untouched modal plays exactly like the
// compile-time tuning. GameSpace::applyOptions() stamps these onto the sim.

#pragma once

#include "constants.h"

#include <string>
#include <unordered_map>

struct MatchOptions {
    int   numPlayers    = GAMESPACE_DEFAULT_PLAYERS; // 1 human + (N-1) bots
    float botDifficulty = BOT_DIFFICULTY_DEFAULT;    // 0.0..BOT_DIFFICULTY

    // Elasticity sliders are PLAYER-only: asteroids keep their compile-time
    // bounce (WALL/PLATFORM_ELASTICITY_ASTEROID) so the asteroid field's feel
    // doesn't change under a bouncy-player match.
    float wallElasticity     = WALL_ELASTICITY_PLAYER;     // 0..1
    float platformElasticity = PLATFORM_ELASTICITY_PLAYER; // 0..1

    // Multipliers over the compile-time tuning. SPEED BOOST is the master
    // scale (walk + jetpack + rocket); ROCKET VELOCITY and JETPACK THRUST
    // stack on top of it for their domains.
    float speedBoost           = 1.0f; // 1..2: walk/jetpack speed+accel, rocket speed
    float rocketSpeedScale     = 1.0f; // 1..2, on top of speedBoost (rockets only)
    float explosionRadiusScale = 1.0f; // 1..4: damage radius + blast visual
    float jetpackThrust        = 1.0f; // 1..2, on top of speedBoost (jetpack only)

    // Fuel: consumption is a direct rate (units/sec out of the 100-unit tank,
    // so it reads as %/sec); regen is a PERCENTAGE of the consumption rate
    // (100 = regen keeps pace with the burn). Ints for clean slider readouts.
    int fuelConsumption = (int)FUEL_CONSUMPTION_RATE; // 0..100 units/sec
    int fuelRegenPct    = FUEL_REGEN_PCT_DEFAULT;     // 0..100 % of consumption

    bool wallsEnabled       = WALLS_ENABLED;
    bool rocketsObeyPhysics = ROCKETS_OBEY_PHYSICS;
    bool friendlyFire       = FRIENDLY_FIRE;
    bool coastMode          = COAST_MODE;
};

//MARK: Match option presets
// A named bundle of match rules, used to seed a new match. Public rooms are
// created from one and then LOCK it (nobody may retune mid-lobby); private rooms
// use it only as the host's starting point.
//
// This is a catalogue of values, not state - which is why it lives here beside
// MatchOptions rather than in the match registry. The registry records WHICH
// preset a room was created from; the values themselves are copied into the
// Match at creation, so the 60 Hz state packet never has to reach across into
// registry storage under the registry lock to build its "opt" block.
//
// One entry for now. Adding CHAOS / LOW-GRAV / etc. is a data change here and
// nothing else.
struct MatchPreset {
    MatchOptions options;
    // Key into mapSizePresets (constants.h). Separate only until B3 folds map
    // size into MatchOptions, at which point this collapses into `options`.
    std::string  mapSize;
};

// MatchOptions{} is exactly the compile-time tuning in constants.h, so DEFAULT
// plays identically to an untouched OPTIONS modal.
inline std::unordered_map<std::string, MatchPreset> matchOptionPresets = {
    {"DEFAULT", { MatchOptions{}, "MEDIUM" }},
};

// Look up a preset by name, falling back to DEFAULT for an unknown one - a bad
// preset name should seed a playable match, never fail a join.
inline const MatchPreset& MatchPresetByName(const std::string& name) {
    auto it = matchOptionPresets.find(name);
    return it != matchOptionPresets.end() ? it->second
                                          : matchOptionPresets.at("DEFAULT");
}
