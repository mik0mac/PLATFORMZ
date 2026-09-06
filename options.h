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

//MARK: Match kind
// How a room is GOVERNED: who may change its rules, and who starts it.
//
// Deliberately independent of VISIBILITY (public vs invite-only), which decides
// only whether the room is advertised in the browser. These were once the same
// flag - public meant locked-and-self-starting, invite-only meant host-run -
// which made a public room with a host impossible to express, and that is exactly
// what a public CUSTOM match is.
//
//   OFFICIAL  fixed preset, nobody may retune it, no host, starts itself once
//             PUBLIC_MIN_PLAYERS arrive. Created by the SERVER only: if a player
//             could mint one, "official rules" would guarantee nothing.
//   CUSTOM    the creator is host and sets the rules, starts and ends it.
//             Public or invite-only, their choice.
enum class MatchKind { Official, Custom };

inline const char* matchKindWire(MatchKind k) {
    return k == MatchKind::Official ? "official" : "custom";
}
// Anything that is not the official token is custom - a room whose governance we
// cannot read should be assumed host-run, never assumed to carry the preset
// guarantee.
inline MatchKind matchKindFromWire(const std::string& s) {
    return s == "official" ? MatchKind::Official : MatchKind::Custom;
}

//MARK: Match option presets
// A named bundle of match rules, used to seed a new match. An OFFICIAL room is
// created from one and then LOCKS it (nobody may retune it, ever); a CUSTOM room
// uses it only as the host's starting point.
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
    // What the browser calls the official room built from this preset. The key
    // ("DEFAULT") names a rule set; this names a place to play, and those want
    // different words in a list a player is reading.
    std::string  label;
};

// MatchOptions{} is exactly the compile-time tuning in constants.h, so DEFAULT
// plays identically to an untouched OPTIONS modal.
inline std::unordered_map<std::string, MatchPreset> matchOptionPresets = {
    {"DEFAULT", { MatchOptions{}, "MEDIUM", "OFFICIAL MATCH" }},
};

// Look up a preset by name, falling back to DEFAULT for an unknown one - a bad
// preset name should seed a playable match, never fail a join.
inline const MatchPreset& MatchPresetByName(const std::string& name) {
    auto it = matchOptionPresets.find(name);
    return it != matchOptionPresets.end() ? it->second
                                          : matchOptionPresets.at("DEFAULT");
}
