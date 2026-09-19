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
// and every rule below already crosses the wire in the state packet's option
// block. **Server rebuild + restart only** - the client never reads this table,
// it only ever sends a preset NAME as a string. What it does NOT cover is a
// preset needing a rule that does not exist yet: a new field means a new wire
// key, both ends, and a web rebuild.
struct MatchPreset {
    // The map used to sit beside this as its own field; it is inside `options`
    // now, so a preset is exactly one bundle of rules.
    MatchOptions options;
    // What the browser calls the official room built from this preset. The key
    // ("DEFAULT") names a rule set; this names a place to play, and those want
    // different words in a list a player is reading.
    // Capped by MATCH_NAME_MAX_CHARS (24) - it is a room name like any other.
    std::string  label;
};

// Start from the defaults and change only what the preset is ABOUT.
//
// Spelling all fifteen fields out positionally would be unreadable, and worse,
// it would silently SHIFT the moment MatchOptions gains a field - every preset
// would keep compiling and mean something else. Naming the fields means a
// preset's definition is exactly its diff from DEFAULT, and a new option lands
// on every preset at its own default until somebody decides otherwise.
template <typename Tune>
inline MatchPreset MakePreset(const char* label, Tune tune) {
    MatchOptions o;      // == the rule defaults above
    tune(o);
    return MatchPreset{o, label};
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
    {"DEFAULT", { MatchOptions{}, "OFFICIAL MATCH" }},   // MatchOptions{}.mapSize == MEDIUM

    // Second because it changes only the DIAL, not the game: rockets still fly
    // straight, walls still hold you in, you just do it louder and with a full
    // house. Nothing here has to be re-learned.
    {"CHAOS", MakePreset("CHAOS MATCH", [](MatchOptions& o) {
        o.mapSize              = "LARGE";
        o.numPlayers           = 8;
        o.botDifficulty        = 0.5f;
        o.speedBoost           = 1.25f;
        o.rocketSpeedScale     = 1.5f;
        o.explosionRadiusScale = 2.5f;   // slider max is 4.0
    })},

    // Third because it changes AIMING. Tight and quick - a small arena, fewer
    // bodies - and rockets that obey gravity and carry the shooter's momentum,
    // so a shot has to be led rather than pointed.
    {"SKIRMISH", MakePreset("SKIRMISH MATCH", [](MatchOptions& o) {
        o.mapSize            = "SMALL";
        o.numPlayers         = 4;
        o.botDifficulty      = 0.35f;
        o.speedBoost         = 1.4f;
        o.jetpackThrust      = 1.4f;
        o.rocketsObeyPhysics = true;     // gravity + inherited launch velocity
    })},

    // Fourth because it changes MOVING, which is more fundamental than aiming.
    // Attrition: hard bots, a big arena, fuel that burns faster than it comes
    // back, and no coasting - release the key and you slow down, so position has
    // to be earned and re-earned.
    {"ENDURANCE", MakePreset("ENDURANCE MATCH", [](MatchOptions& o) {
        o.mapSize         = "XL";
        o.numPlayers      = 8;
        o.botDifficulty   = 0.7f;
        o.fuelConsumption = 9;           // default 5 units/sec
        o.fuelRegenPct    = 20;          // default 40% of consumption
        o.coastMode       = false;       // friction on: release the key, slow down
    })},

    // LAST, because it takes away the arena itself - the one thing every other
    // entry above still has. Nothing collides with the cube: rockets fade past
    // the line, asteroids wrap through the origin, and a player who drifts out is
    // eliminated on the out-of-bounds timer, which is what still ends a match in
    // a room with no walls to pin anyone in.
    {"VOID", MakePreset("OPEN SPACE MATCH", [](MatchOptions& o) {
        o.mapSize       = "XL";
        o.numPlayers    = 6;
        o.botDifficulty = 0.3f;
        o.wallsEnabled  = false;
    })},
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
