// options.h - everything that defines WHAT A MATCH IS, in one file:
//
//   the arenas          mapSizePresets + the append-only mapSizeOrder
//   the rules           MatchOptions, the full set the OPTIONS modal drives
//   their defaults      the compile-time value of every one of those rules
//   the named variants  matchOptionPresets, ordered typical-first
//
// One struct so the modal, startGame, the wire (serializeOptions/serializeStart
// in wire.h), and the server's apply-on-start all pass the same bundle instead
// of an ever-growing positional arg list. An untouched modal plays exactly the
// defaults below. GameSpace::applyOptions() stamps them onto the sim.
//
// The defaults used to live in constants.h, spread across seven sections that
// had nothing else to do with each other. constants.h keeps everything a match
// CANNOT retune - geometry, capacities, VFX, audio, network and the sim tuning.
//
//MARK: Do not add repo #includes to this file
// `elements.h` includes this header, so anything added here lands in the lowest
// layer of the game and everything downstream of it. `constants.h` is the only
// repo header it may include - and the dependency runs one way, options.h ->
// constants.h, never back. An `#include "gamespace.h"` here would be an outright
// cycle. If this file ever genuinely needs more, split the defaults into their
// own header rather than widening this one.

#pragma once

#include "constants.h"

#include <string>
#include <unordered_map>
#include <utility>       // std::pair - matchOptionPresets is an ORDERED list
#include <vector>

//MAKR: TESTING BOOL
// true = matches start with one human player
const bool PRESET_TESTING_MODE = true;

//MARK: Map sizes
// Which arenas exist. Moved here from constants.h with the rule defaults below:
// a match's arena is one of its rules, and the OPTIONS map selector sits in the
// same modal as every other control.
struct mapSizePreset {
    float halfSize;
    int numPlatforms;
    int numAsteroids;
};

// inline: one definition shared across all TUs (options.h is included by
// main.cpp, elements.h, the server, ...). Can't be const - main.cpp uses operator[].
inline std::unordered_map<std::string, mapSizePreset> mapSizePresets = {
    {"SMALL",  {90.0f, 64, 12}},
    {"MEDIUM", {120.0f, 128, 18}},
    {"LARGE",  {240.0f, 256, 24}},
    // XL: 27x LARGE volume; benched 2026-07 on Mike's Mac at p95 ~8ms with bots
    // and rocket fire (framerate was never the binding limit - even 480/1024
    // passed). Platforms scale with AREA, not volume (they're a traversal
    // surface; volume-scaling would blow the render batch). Asteroid counts
    // here are the LOCAL-play numbers; a networked start clamps them to the
    // UDP state-packet budget for the roster size (nb::MaxAsteroidsForRoster,
    // netbin.h) so a full tick fits one unfragmented datagram.
    {"XL",     {360.0f, 576, 36}}
};

// WIRE ORDER for the presets above. mapSizePresets is an unordered_map and so
// has no stable iteration order, but a map choice crosses the wire as an INDEX
// into this list - two bits inside the options flags byte, which is why adding
// it cost no protocol version bump.
//
// APPEND ONLY. Reordering silently reinterprets every connected client's choice,
// and a fifth entry needs a third bit (the flags byte has 64 and 128 free).
//
// Note this is the OPPOSITE of matchOptionPresets below, whose order is free to
// change precisely because a preset crosses the wire as a NAME, not an index.
// Two tables in one file with two different rules about reordering - the
// difference is entirely what travels over the network.
//
// A fifth entry also needs wire.h's `(optFlags >> 4) & 0x3`, which hardcodes the
// mask rather than deriving it from MAP_SIZE_BITS. Pre-existing; noted because
// the two now sit in different files with no textual link.
inline const char* const mapSizeOrder[] = { "SMALL", "MEDIUM", "LARGE", "XL" };
inline constexpr int MAP_SIZE_COUNT = 4;
inline constexpr int MAP_SIZE_BITS  = 2;   // enough for MAP_SIZE_COUNT
static_assert(MAP_SIZE_COUNT <= (1 << MAP_SIZE_BITS),
              "the map index no longer fits the bits reserved in the options flags byte");

// Unknown names fall back to MEDIUM rather than failing: a bad map should give a
// playable match, never a refused start.
inline int MapSizeIndex(const std::string& name) {
    for (int i = 0; i < MAP_SIZE_COUNT; ++i)
        if (name == mapSizeOrder[i]) return i;
    return 1; // MEDIUM
}
inline const char* MapSizeName(int index) {
    return (index >= 0 && index < MAP_SIZE_COUNT) ? mapSizeOrder[index] : mapSizeOrder[1];
}

//MARK: Match rule defaults
// The compile-time value of every rule a match can retune, in the same order as
// the MatchOptions fields below - so "is every default accounted for?" is a
// visual scan against the struct rather than a grep.
//
// These moved out of constants.h, which keeps everything a match CANNOT change.
// Where a pair got split, the survivor carries a pointer comment: the PLAYER
// elasticities are rules and live here, the ASTEROID ones are compile-time and
// stayed; BOT_DIFFICULTY_DEFAULT is here, its BOT_DIFFICULTY cap stayed.

// numPlayers. The ceiling is GAMESPACE_NUMBER_OF_PLAYERS (constants.h), which is
// a capacity rather than a rule - it sizes a std::array and a static_assert on
// the server, so it cannot live here.
const int GAMESPACE_DEFAULT_PLAYERS = 4; // Default NUMBER OF PLAYERS - what the OPTIONS slider and the server's pending config start at; the host can raise it to the ceiling above.

// botDifficulty. Centre of the personality distribution; each bot is seeded from
// its player.id as this +/- a spread. The MAX is BOT_DIFFICULTY (constants.h).
const float BOT_DIFFICULTY_DEFAULT = 0.2f; // OPTIONS starting value for BOT DIFFICULTY (client + server defaults)

// maxBots. One short of the roster ceiling, so a match is never ALL bots and the
// old "every unclaimed slot becomes a bot" behaviour is preserved for every
// roster size a player can pick (numPlayers - 1 bots is at most 7).
//
// The one place this differs from the old behaviour: a room with ZERO humans
// connected previews 7 bots and one empty slot where it used to preview 8.
// Nobody is connected to see it.
const int MAX_BOTS_DEFAULT = GAMESPACE_NUMBER_OF_PLAYERS - 1;

// minHumansToStart's default is PUBLIC_MIN_PLAYERS (constants.h), which is what
// every room used before a preset could ask for more.

// wallElasticity / platformElasticity. PLAYER-only: asteroids keep their
// compile-time bounce (WALL/PLATFORM_ELASTICITY_ASTEROID, constants.h) so the
// asteroid field's feel doesn't change under a bouncy-player match.
const float WALL_ELASTICITY_PLAYER = 0.5f; // hit velocity is reflected and scaled by this (velocity = -velocity * elasticity)
const float PLATFORM_ELASTICITY_PLAYER = 0.33f; // For bouncy platforms, 0.0 - 1.0, determines how much the player bounces (velocity = -velocity * elasticity)

// fuelConsumption / fuelRegenPct. FUEL CONSUMPTION is a direct units/sec value
// (tank is 100, so it reads as %/sec); FUEL REGEN is a percentage OF that rate,
// so the 40% default recreates the old 2/sec regen (40% of 5).
const float FUEL_CONSUMPTION_RATE = 5.0f; // Per sec; default for the OPTIONS slider.
const int FUEL_REGEN_PCT_DEFAULT = 40; // Regen as % of consumption; default for the OPTIONS slider.

// wallsEnabled
const bool WALLS_ENABLED = true; // if true, the boundary walls are drawn and everything collides with them (rockets detonate). If false, nothing collides: rockets fade out past the boundary and players are subject to the out-of-bounds elimination rules.

// rocketsObeyPhysics. OPTIONS "ROCKETS OBEY PHYSICS": one match-wide toggle that
// drives BOTH rocket gravity and shooter-velocity inheritance (input.h sets each
// fired rocket's gravityEnabled/velocityInheritance from
// GameSpace::rocketsObeyPhysics). The per-rocket defaults it overrides,
// ROCKET_GRAVITY_ENABLED and ROCKET_VELOCITY_INHERITANCE_ENABLED, stayed in
// constants.h.
const bool ROCKETS_OBEY_PHYSICS = true; // default ON. OFF = rockets fly straight, no inherited velocity.

// friendlyFire. When OFF, a player takes no splash DAMAGE from their own blast
// (self-knockback still applies, so rocket-jumping survives). Default ON keeps
// the current behavior. Consumed in ApplyExplosionSplashDamage (collisions.cpp).
const bool FRIENDLY_FIRE = true; // default ON: your own rocket can damage you (current behavior)

// coastMode. ON = frictionless: releasing the keys coasts instead of easing to a
// stop, and momentum is never braked away just because the speed cap dropped
// (jetpack released, tank empty). Thrust can still redirect that momentum, it
// just can't grow it past the cap. See the two branches in
// Player::updateVelocity.
const bool COAST_MODE = true;

struct MatchOptions {
    // Which arena. Lives HERE rather than being implied by whichever of four
    // START buttons got pressed, because a lobby everyone can see has to show
    // the map BEFORE the match starts - and the browser wants to advertise it.
    // Crosses the wire as an index into mapSizeOrder (above).
    std::string mapSize = "MEDIUM";

    int   numPlayers    = GAMESPACE_DEFAULT_PLAYERS; // roster size: humans + bots + empty slots
    float botDifficulty = BOT_DIFFICULTY_DEFAULT;    // 0.0..BOT_DIFFICULTY

    // How many UNCLAIMED slots get filled with a bot. This does NOT cap the
    // room's capacity - the roster is still numPlayers slots and a human can
    // take any of them - it caps how much of an empty room gets papered over.
    // Slots past the cap stay genuinely EMPTY: no body in the arena, nothing to
    // shoot, not counted for last-man-standing, and still joinable. 0 = a
    // humans-only room.
    //
    // The two below are the only rules with NO slider in the OPTIONS modal: they
    // are things a room's AUTHOR decides, not things a player dials mid-lobby.
    // They live here anyway, so a preset is one bundle of rules written one way.
    int maxBots = MAX_BOTS_DEFAULT;

    // How many HUMANS an official room needs before its auto-start countdown
    // arms. Meaningless in a custom room (its host presses START) and in local
    // play; the lobby reads it to say how many more players it is waiting for.
    // Must be <= numPlayers or the room could never start - an authoring rule
    // asserted over every preset in registry_test.cpp.
    int minHumansToStart = PUBLIC_MIN_PLAYERS;

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
    float explosionRadiusScale = 2.0f; // 1..4: damage radius + blast visual
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

//MARK: Legal ranges - ONE definition, three consumers
// What counts as a legal value for each rule. Three places need to agree on
// this, and until now all three hand-kept their own copy of the numbers:
//
//   1. the OPTIONS modal's sliders          (screens.h)
//   2. the profile's sanitiser              (profile::SanitizeOptions)
//   3. the preset values above, which set fields directly and bypass both
//
// Nothing linked them. Widen a slider's max and forget the clamp, and the new
// range works all session, gets written to profile.json, and is silently dragged
// back on the NEXT launch - which reads as "the game forgot my settings" rather
// than as the edit it actually was. Widen the clamp and forget the slider, and
// the extra range is simply unreachable. Neither produces a compile error, and
// the two files are far enough apart that nobody reads them together.
//
// So the ranges live here, beside the struct they constrain and the defaults
// they must contain, and the other two read them.
struct OptionRange {
    float min;
    float max;
    // 0 = continuous. The three sliders backed by ints snap to 1 so the readout
    // can't show a value the field cannot hold; UiSlider takes this directly.
    float step;

    float clampf(float v) const { return v < min ? min : (v > max ? max : v); }
    int   clampi(int v)   const { return (int)clampf((float)v); }
    // Used by the tests to prove every preset is dialable in the UI.
    bool  holds(float v)  const { return v >= min && v <= max; }
};

// In MatchOptions field order, same as the defaults above. mapSize has no range
// - it is a name, validated against mapSizePresets instead (see ClampOptions).
const OptionRange OPT_RANGE_NUM_PLAYERS         = { 1.0f, (float)GAMESPACE_NUMBER_OF_PLAYERS, 1.0f };
const OptionRange OPT_RANGE_BOT_DIFFICULTY      = { 0.0f, BOT_DIFFICULTY, 0.0f };
// No slider reads these two - they have no row in the OPTIONS modal - but they
// are still rules that arrive off the wire and out of profile.json, so they get
// the same clamp as everything else. A minimum of 1 human is what stops a preset
// from authoring a room that can never start.
const OptionRange OPT_RANGE_MAX_BOTS            = { 0.0f, (float)GAMESPACE_NUMBER_OF_PLAYERS, 1.0f };
const OptionRange OPT_RANGE_MIN_HUMANS          = { 1.0f, (float)GAMESPACE_NUMBER_OF_PLAYERS, 1.0f };
const OptionRange OPT_RANGE_WALL_ELASTICITY     = { 0.0f, 1.0f,   0.0f };
const OptionRange OPT_RANGE_PLATFORM_ELASTICITY = { 0.0f, 1.0f,   0.0f };
const OptionRange OPT_RANGE_SPEED_BOOST         = { 1.0f, 2.0f,   0.0f };
const OptionRange OPT_RANGE_ROCKET_SPEED        = { 1.0f, 2.0f,   0.0f };
const OptionRange OPT_RANGE_EXPLOSION_RADIUS    = { 1.0f, 4.0f,   0.0f };
const OptionRange OPT_RANGE_JETPACK_THRUST      = { 1.0f, 2.0f,   0.0f };
const OptionRange OPT_RANGE_FUEL_CONSUMPTION    = { 0.0f, 100.0f, 1.0f };
const OptionRange OPT_RANGE_FUEL_REGEN          = { 0.0f, 100.0f, 1.0f };

// Force every rule into its legal range. Lives here rather than in profile.h
// because the ranges do: a profile is only one of the things that can carry an
// out-of-range value (a hand-edited file, an older build's bundle, a future
// preset). profile::SanitizeOptions delegates to this.
//
// The bools are deliberately absent - a bool has no invalid value.
inline void ClampOptions(MatchOptions& m) {
    // Not a range: an arena is named, so the test is membership. An unknown name
    // falls back to the default rather than failing, same rule as MapSizeIndex.
    if (mapSizePresets.find(m.mapSize) == mapSizePresets.end()) m.mapSize = MatchOptions{}.mapSize;
    m.numPlayers           = OPT_RANGE_NUM_PLAYERS.clampi(m.numPlayers);
    m.botDifficulty        = OPT_RANGE_BOT_DIFFICULTY.clampf(m.botDifficulty);
    m.maxBots              = OPT_RANGE_MAX_BOTS.clampi(m.maxBots);
    m.minHumansToStart     = OPT_RANGE_MIN_HUMANS.clampi(m.minHumansToStart);
    m.wallElasticity       = OPT_RANGE_WALL_ELASTICITY.clampf(m.wallElasticity);
    m.platformElasticity   = OPT_RANGE_PLATFORM_ELASTICITY.clampf(m.platformElasticity);
    m.speedBoost           = OPT_RANGE_SPEED_BOOST.clampf(m.speedBoost);
    m.rocketSpeedScale     = OPT_RANGE_ROCKET_SPEED.clampf(m.rocketSpeedScale);
    m.explosionRadiusScale = OPT_RANGE_EXPLOSION_RADIUS.clampf(m.explosionRadiusScale);
    m.jetpackThrust        = OPT_RANGE_JETPACK_THRUST.clampf(m.jetpackThrust);
    m.fuelConsumption      = OPT_RANGE_FUEL_CONSUMPTION.clampi(m.fuelConsumption);
    m.fuelRegenPct         = OPT_RANGE_FUEL_REGEN.clampi(m.fuelRegenPct);
}

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
// Adding one is a data change here and nothing else: the server's boot loop
// creates a pinned OFFICIAL room per entry, the browser advertises it by `label`,
// and every rule in `options` already crosses the wire in the state packet's
// option block. **Server rebuild + restart only** - the client never reads this
// table, it only ever sends a preset NAME as a string. What it does NOT cover is
// a preset needing a rule that does not exist yet: a new field means a new wire
// key, both ends, and a web rebuild.
struct MatchPreset {
    // The map used to sit beside this as its own field; it is inside `options`
    // now, so a preset is exactly one bundle of rules.
    MatchOptions options;
    // What the browser calls the official room built from this preset. The key
    // ("DEFAULT") names a rule set; this names a place to play, and those want
    // different words in a list a player is reading.
    // Capped by MATCH_NAME_MAX_CHARS (24) - it is a room name like any other.
    std::string label;
    // info to display the player about the preset.
    std::string description;
};

// Start from the defaults and change only what the preset is ABOUT.
//
// Spelling all seventeen fields out positionally would be unreadable, and worse,
// it would silently SHIFT the moment MatchOptions gains a field - every preset
// would keep compiling and mean something else. Naming the fields means a
// preset's definition is exactly its diff from DEFAULT, and a new option lands
// on every preset at its own default until somebody decides otherwise.
template <typename Tune>
inline MatchPreset MakePreset(const char* label, Tune tune, std::string description = "") {
    MatchOptions o;      // == the rule defaults above
    tune(o);
    return MatchPreset{o, label, description};
}

//MARK: ORDER IS MEANINGFUL - typical gameplay first, niche last
// Read this list as a ramp. The FIRST entry is the game as it is meant to be
// played by someone who has never played it; each one after it departs further
// from that, and the LAST is the most specialised thing on offer. A new variant
// goes where its strangeness puts it, not on the end.
//
// This is not a style preference, it is behaviour. When several official rooms
// are equally full - which, on a quiet server, is all of them at zero - QUICK
// MATCH walks this list and takes the first match it finds (see
// server_main.cpp). So position here decides what a brand-new player is dropped
// into, and a niche room sitting at the top would make the weirdest variant the
// front door. It is also the order the resident rooms are created in at boot.
//
// A VECTOR, not the unordered_map this used to be, because that had no stable
// iteration order at all: rooms came out in hash order and QUICK MATCH's choice
// changed between builds. Deliberately NOT the mapSizePresets + mapSizeOrder
// pattern at the top of this file - that one needs a separate array because a map choice
// crosses the wire as an INDEX into it, so the order is an append-only protocol
// contract. A preset's NAME crosses the wire instead, so a second list would buy
// nothing here and could silently fall out of step with this one. Lookup is a
// linear scan over a handful of entries, on the room-creation path.
//
// MatchOptions{} is exactly the rule defaults above, so DEFAULT
// plays identically to an untouched OPTIONS modal.
//
//MARK: The four after DEFAULT are PLACEHOLDERS
// Their shapes are deliberate - between them they exercise every toggle and the
// main sliders, so the plumbing is actually proven rather than five rooms that
// all play identically - but the NUMBERS are untuned guesses. Nobody has played
// these. Treat any value here as a starting point for tuning, not as a decision.
inline std::vector<std::pair<std::string, MatchPreset>> matchOptionPresets = {
    // The standard game, and the front door: what QUICK MATCH hands a stranger.
    // The label must remain DEFAUT so the serer knows it is where a player goes
    // on quick match all other options being equal
    {"DEFAULT", MakePreset("CLASSIC", [](MatchOptions& o) {
        o.mapSize              = "MEDIUM";
        o.numPlayers           = 6;
        o.maxBots              = 2;
        o.minHumansToStart     = (PRESET_TESTING_MODE) ? 1 : 2;
    }, "The default setup.  MEDIUM map.")},   // MatchOptions{}.mapSize == MEDIUM

    {"CLASSIC HYPED", MakePreset("CLASSIC HYPED", [](MatchOptions& o) {
        o.mapSize              = "LARGE";
        o.numPlayers           = 8;
        o.minHumansToStart     = (PRESET_TESTING_MODE) ? 1 : 2;
        o.maxBots              = 2;
        o.botDifficulty        = 0.5f;
        o.speedBoost           = 1.75f;
        o.rocketSpeedScale     = 1.5f;
        o.explosionRadiusScale = 3.0f;   // slider max is 4.0
    }, "Boosted version of classic.  LARGE map.")},

    {"SPAMMERS DELIGHT", MakePreset("SPAMMERS DELIGHT", [](MatchOptions& o) {
        o.mapSize              = "LARGE";
        o.numPlayers           = 6;
        o.minHumansToStart     = (PRESET_TESTING_MODE) ? 1 : 2;
        o.maxBots              = 2;
        o.botDifficulty        = 0.35f;
        o.speedBoost           = 1.2f;
        o.jetpackThrust        = 1.2f;
        o.friendlyFire         = false;
        o.explosionRadiusScale = 3.0f;
        o.rocketSpeedScale     = 2.0f;
        o.coastMode            = false;      // friction on: release the key, slow down
    }, "Friendly fire off and speed boosted.  No coast mode.")},

    {"MAYHEM", MakePreset("MAYHEM", [](MatchOptions& o) {
        o.mapSize              = "SMALL";
        o.numPlayers           = 4;
        o.minHumansToStart     = (PRESET_TESTING_MODE) ? 1 : 2;
        o.botDifficulty        = 0.7f;
        o.maxBots              = 2;          
        o.friendlyFire         = false;
        o.explosionRadiusScale = 4.0f;
        o.speedBoost           = 2.0f;
    }, "Super big and super fast on a SMALL map.")},

    {"VOID", MakePreset("THE VOID", [](MatchOptions& o) {
        o.mapSize               = "XL";
        o.numPlayers            = 8;
        o.minHumansToStart      = (PRESET_TESTING_MODE) ? 1 : 3; // an empty void is a dull one
        o.botDifficulty         = 0.35f;
        o.maxBots               = 3;
        o.fuelConsumption       = 50.0f;
        o.fuelRegenPct          = 10.0f;
        o.platformElasticity    = 0.9f;
        o.explosionRadiusScale  = 4.0f;
        o.rocketSpeedScale      = 2.0f;
        o.friendlyFire          = false;
        o.wallsEnabled          = false;
    }, "XL map, no walls.  Fuel is scarce. Friendly fire off.")}
};

// Where a preset sits on the typical-to-niche ramp above; matchOptionPresets
// .size() for one that is not in the table, so an unknown name sorts LAST rather
// than accidentally winning a tie. This is what QUICK MATCH breaks ties on.
inline size_t MatchPresetRank(const std::string& name) {
    for (size_t i = 0; i < matchOptionPresets.size(); ++i)
        if (matchOptionPresets[i].first == name) return i;
    return matchOptionPresets.size();
}

// Look up a preset by name, falling back to DEFAULT for an unknown one - a bad
// preset name should seed a playable match, never fail a join.
//
// A linear scan, because the table is ordered (see above) and holds a handful of
// entries. It runs on room CREATION, not on any per-tick path.
inline const MatchPreset& MatchPresetByName(const std::string& name) {
    for (const auto& [key, preset] : matchOptionPresets)
        if (key == name) return preset;
    // DEFAULT is first by construction - it is the front door of the ramp - but
    // find it by name rather than trusting [0], so a reorder cannot turn the
    // fallback into whatever happens to be on top.
    for (const auto& [key, preset] : matchOptionPresets)
        if (key == "DEFAULT") return preset;
    return matchOptionPresets.front().second;   // no DEFAULT at all: still playable
}
