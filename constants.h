#pragma once

#include <unordered_map> // mapSizePresets
#include <string>        // mapSizePresets key

//MARK: Audio events (shared client+server wire contract)
// Small-int ids so the headless server can tag/serialize events without any
// raylib audio. The client maps each id to a real audioFX via its fxTable.
enum AudioFXId {
    FX_ROCKET_LAUNCH = 0,
    FX_EXPLOSION,
    FX_ASTEROID_BONUS,
    FX_PLAYER_HIT,
    FX_PLAYER_DEATH,
    FX_NO_AMMO,
    FX_NO_FUEL,
    FX_FIRERATE_CHOKE,
    FX_WALL_BOUNCE_PLAYER,
    FX_ROCKET_THROUGH_WALL,
    FX_PLATFORM_PASSTHROUGH,
    FX_MESSAGE_RECEIVED,
    FX_PLAYER_ELIMINATION_SCORE,
    FX_PLAYER_LOCAL_DAMAGE,
    FX_WARNING,
    FX_ENGAGE_EARTH_GRAVITY,
    FX_COUNT
};

const float AUDIO_MAX_DISTANCE = 80.0f; // max distance for audio attenuation.  The distance at which the sound is the softest.
const float AUDIO_MIN_VOLUME = 0.125f; // minimum volume for audio attenuation.  The volume at the max distance.

//MARK: Message events
enum MessageType {
    MSG_TYPE_LOW_FUEL,
    MSG_TYPE_LOW_AMMO,
    MSG_TYPE_LOW_HEALTH,
    MSG_TYPE_EXPLOSION_HIT,
    MSG_TYPE_PLAYER_COLLISION,
    MSG_TYPE_ASTEROID_COLLISION,
    MSG_TYPE_ELIMINATION,
    MSG_TYPE_ASTEROID_BONUS,
    COUNT
};

//MARK: Physics Constants
const float MOON_GRAVITY = 3.25f; //1.62f; // moon gravity, m/s^2 (assuming 1 unit = 1 meter)
const float EARTH_GRAVITY = 19.6133f; //9.81f; // earth gravity, m/s^2 * 2

//MARK: GameSpace Constants
// these only used as class defaults and are overwritten by the mapSizePresets in main.cpp.
const float GAMESPACE_HALF_SIZE = 60.0f; // half-size of the game space cube, units.  Also used by Walls.
const int GAMESPACE_NUMBER_OF_PLATFORMS = 36; // Number of platforms in the game space
const int GAMESPACE_NUMBER_OF_ASTEROIDS = 18; // Number of asteroids in the game space
const int GAMESPACE_NUMBER_OF_PLAYERS = 4; // Number of players (index 0 is the local human; 1+ are wander-bots for testing)

struct mapSizePreset {
    float halfSize;
    int numPlatforms;
    int numAsteroids;
};

// inline: one definition shared across all TUs (constants.h is included by
// main.cpp, collisions.cpp, ...). Can't be const - main.cpp uses operator[].
inline std::unordered_map<std::string, mapSizePreset> mapSizePresets = {
    {"SMALL",  {60.0f, 32, 6}}, // 8 platforms, 4 asteroids
    {"MEDIUM", {90.0f, 64, 12}}, // 32 platforms, 12 asteroids
    {"LARGE",  {120.0f, 128, 24}}  // 128 platforms, 24 asteroids
};

const float GAME_OVER_TIMER = 5.0f; // seconds to wait before showing the game-over screen after the last player dies
const float COUNTDOWN_SECONDS = 5.0f; // "GAME STARTING IN..." pre-match countdown; world is built but frozen until it hits zero. Shared by client (local timer) and server (networked deadline) so both agree.

//MARK: Wall Constants
const float WALL_ELASTICITY_PLAYER = 0.9f; // hit velocity is reflected and scaled by this (velocity = -velocity * elasticity)
const float WALL_ELASTICITY_ASTEROID = 0.95f; // hit velocity is reflected and scaled by this (velocity = -velocity * elasticity)
const int WALL_DAMAGE = 0; // damage dealt to the player on wall impact
const bool WALLS_STOP_ROCKETS = false; // if true, rockets are destroyed on wall impact; if false, they fly through.

//MARK: Platform Constants
const float PLATFORM_ELASTICITY_PLAYER = 0.33f; // For bouncy platforms, 0.0 - 1.0, determines how much the player bounces (velocity = -velocity * elasticity)
const float PLATFORM_ELASTICITY_ASTEROID = 0.99f; // For bouncy platforms, 0.0 - 1.0, determines how much the asteroid bounces (velocity = -velocity * elasticity)

const float PLATFORM_MIN_WIDTH = 12.0f; //GAMESPACE_HALF_SIZE / 12.0f; // 5.0f; // Minimum width of the platform
const float PLATFORM_MAX_WIDTH = 18.0f; //GAMESPACE_HALF_SIZE / 3.0f; // 20.0f; // Maximum width of the platform
const float PLATFORM_MIN_HEIGHT = 0.5f; // Minimum height of the platform
const float PLATFORM_MAX_HEIGHT = 0.5f; // Maximum height of the platform
const float PLATFORM_MIN_DEPTH = PLATFORM_MIN_WIDTH; // Minimum depth of the platform
const float PLATFORM_MAX_DEPTH = PLATFORM_MAX_WIDTH; // Maximum depth of the platform

const bool EARTH_GRAVITY_PASS_THROUGH_PLATFORMS = true; // If true, players pass through platforms when Earth gravity is enabled. Default for the OPTIONS toggle.


//MARK: Player Shape / Size
// The player renders as a regular dodecahedron (DrawPlayer in shapes.h). Earlier
// prototype silhouettes (DART, DELTA, LANDER, POD) are archived in
// docs/player-shapes-archive.md.

// The player is a sphere (rendered as a dodecahedron, DrawPlayer in shapes.h)
// centered on player.position; the camera sits at that center. PLAYER_SCALE
// tunes the size about the center point.
const float PLAYER_SCALE = 2.0f;         // proportional size multiplier
const float PLAYER_BASE_RADIUS = 1.0f;   // body/collider radius in units, before scale
const float PLAYER_RADIUS = PLAYER_BASE_RADIUS * PLAYER_SCALE; // == 2.0 (preserves current size)

const int PLAYER_DAMAGE = 10; // Damage dealt to other players or asteroids when colliding.

//MARK: Reticle Constants
// The reticle is a purely visual in-world object (no collision): the player's
// aiming device and, to others, an indicator of where they are looking. It is
// drawn perpendicular to the aim, standing off just beyond the body surface.
enum ReticleShape { RETICLE_SHAPE_CROSSHAIR = 0, RETICLE_SHAPE_BRACKETS = 1 };
const ReticleShape RETICLE_SHAPE = RETICLE_SHAPE_BRACKETS; // toggle the silhouette

const float RETICLE_SIZE = 0.5f;            // radius of the ring / bracket frame, units
const float RETICLE_STANDOFF = 1.5f;        // gap beyond the body surface, along aim
const int   RETICLE_RING_SEGMENTS = 28;     // circle smoothness (crosshair style)
const float RETICLE_CROSS_GAP = 0.35f;      // center gap, as a fraction of RETICLE_SIZE
const float RETICLE_PRONG_LENGTH = 0.6f;    // forward prong length, fraction of RETICLE_SIZE
const float RETICLE_BRACKET_LENGTH = 0.4f;  // bracket arm length, fraction of RETICLE_SIZE
const float RETICLE_SMOOTHING = 12.0f;      // anchor easing rate (1/sec) for non-local reticles; higher = snappier, lower = floatier

//MARK: Player Speed
const float PLAYER_SPEED_WALK = 18.0f; // units/sec
const float PLAYER_ACCELERATION_WALK = 18.0f; // units/sec^2
const float PLAYER_SPEED_JETPACK = 24.0f; // units/sec
const float PLAYER_ACCELERATION_JETPACK = 24.0f; // units/sec^2

//MARK: Health, Ammo, Fuel
const int PLAYER_MAX_AMMO = 100;
const int PLAYER_STARTING_AMMO = PLAYER_MAX_AMMO;
const int PLAYER_MAX_HEALTH = 100;
const int PLAYER_STARTING_HEALTH = PLAYER_MAX_HEALTH;
const float PLAYER_MAX_FUEL = 100.0f;
const float PLAYER_STARTING_FUEL = PLAYER_MAX_FUEL;

const float FUEL_CONSUMPTION_RATE = 5.0f; // Per sec.
const float FUEL_REGEN_RATE = 0.5f; // Per sec.
const float JETPACK_MIN_FUEL = 0.1f; // Min fuel to produce jetpack thrust; below this the tank reads empty.
const float NO_FUEL_SFX_INTERVAL = 1.0f; // Min seconds between "empty tank" cues while jetpack is held on empty.

// Low-resource warning cue (FX_WARNING): alarm pulses while any of these is at/below threshold.
const int   WARN_HEALTH_THRESHOLD   = 25;    // health at/below this pulses the warning alarm
const float WARN_FUEL_THRESHOLD     = 20.0f; // fuel at/below this pulses the warning alarm
const int   WARN_AMMO_THRESHOLD     = 10;    // ammo at/below this pulses the warning alarm
const float WARNING_SFX_INTERVAL    = 1.5f;  // min seconds between warning pulses while a resource stays low
const float EARTH_GRAV_SFX_INTERVAL = 0.5f;  // min seconds between earth-gravity engage cues (guards rapid taps)
const float PLAYER_FIRE_RATE = 3.0f; // shots per second.

const int PLAYER_ELIMINATION_SCORE_AWARD = 100; // Points awarded to player for eliminating another player

const std::vector<Color> HUMAN_PLAYER_COLORS = {
    {255, 0, 0, 255},     // Red
    {0, 0, 255, 255},     // Blue
    {0, 255, 0, 255},     // Green
    {255, 0, 255, 255},   // Pink
};

//MARK: Bot Constants
// Test-only "wander" bots that drive non-local players (index 1+) through the
// same ApplyPlayerInput() path a human/network player uses (see input.h).
const float BOT_REROLL_MIN_SECONDS = 1.5f; // min time before a bot re-rolls its heading/movement
const float BOT_REROLL_MAX_SECONDS = 4.0f; // max time before a bot re-rolls its heading/movement
const float BOT_TURN_RATE = 2.0f; // max yaw the bot turns toward its target heading, radians/sec

// Difficulty & personality (behaviour-tree bots, bot_logic.h). BOT_DIFFICULTY is
// the center of the personality distribution [0..1]; each bot's aggression &
// accuracy are seeded from its player.id as difficulty +/- a spread that widens
// with bot count (so a lone bot ~= difficulty, a crowd is varied).
const float BOT_DIFFICULTY = 1.0f;         // 0 easy .. 1 hard; also the BOT DIFFICULTY slider MAX / hard cap
const float BOT_DIFFICULTY_DEFAULT = 0.2f; // OPTIONS starting value for BOT DIFFICULTY (client + server defaults)
const float BOT_PERSONALITY_SPREAD = 0.2f; // max +/- jitter around difficulty (at high bot counts)
const float BOT_MAX_AIM_SPREAD = 0.50f;    // radians of aim error at accuracy=0 (0 at accuracy=1) 0.5 = ~28.6 degrees 0.3 = ~17.2 degrees
const float BOT_TICK_JITTER_MIN = 0.8f;    // decision-interval multiplier lo (LatchedSelector)
const float BOT_TICK_JITTER_MAX = 1.3f;    // decision-interval multiplier hi

const bool DISABLE_BOT_MOVEMENT = false; // If true, bots don't move or jetpack (for testing other features)
const bool DISABLE_BOT_FIRE_PLAYER = false;     // If true, bots don't fire rockets (for testing other features)
const bool DISABLE_BOT_FIRE_ASTEROIDS = false; // If true, bots don't fire at asteroids (for testing other features)
const float BOT_FIRERATE_MAX = PLAYER_FIRE_RATE; // max shots/sec for bots (lower = easier)
const float BOT_FIRERATE_MIN = PLAYER_FIRE_RATE / 4.0f; // min shots/sec for bots (higher = harder)

// Placeholder bot display names (NATO phonetic alphabet). Used by the title
// screen's players panel to label bot-filled slots.
const char* const BOT_NAME_STRINGS[] = {
    "RoyBOT OVERLORD","Jeff","BOTtholomew","Geoff","Qeff","Djeff","Jeffrey","Jefferson","Jeffery","Geoffrey","Geofferson","Geff","Jef","Jeferson","Jefrey","Jeferson","Jefri","Jefrierson","Jefriani","Jefrianius","Jefrianius Maximus"
};
const int BOT_NAME_COUNT = sizeof(BOT_NAME_STRINGS) / sizeof(BOT_NAME_STRINGS[0]);

const Color BOT_OUTLINE_COLOR = {255, 0, 200, 255}; // Magenta outline for bots.
const Color BOT_FILL_COLOR = {255, 0, 200, 40}; // Magenta fill for bots.

//MARK: Asteroid Constants
const int ASTEROID_STARTING_HEALTH = 50;
const int ASTEROID_DAMAGE = 20; // Damage dealt to the player on asteroid impact
const int ASTEROID_SCORE_AWARD = 25; // Points awarded to player for destroying an asteroid
const float ASTEROID_FUEL_AWARD = 50.0f;
const int ASTEROID_HEALTH_AWARD = 25; // Health awarded to player for destroying an asteroid
const int ASTEROID_AMMO_AWARD = 20;

const float ASTEROID_MIN_SIZE = 6.0f; // Minimum radius of the asteroid
const float ASTEROID_MAX_SIZE = 6.0f; // GAMESPACE_HALF_SIZE / 5.0f; // Maximum radius of the asteroid
const float ASTEROID_MIN_SPEED = 6.0f; // Minimum starting speed of the asteroid
const float ASTEROID_MAX_SPEED = 12.0f; // Maximum starting speed of the asteroid
const float ASTEROID_SPEED_LIMIT = 42.0f; // when bouncing off walls, this speed limit is enforced to avoid runaway velocity with elasticity > 1.0.
// Min center-to-center distance to keep a freshly-scattered asteroid away from any
// player's spawn (issue #5). ~= max asteroid radius (8) + player radius (2) + margin.
const float ASTEROID_PLAYER_SPAWN_BUFFER = 15.0f;

const float ASTEROID_FLASH_DURATION = 4.0f; // length of the hot-glow damage flash, seconds
const float ASTEROID_FLASH_INTENSITY = 0.6f; // intensity of the hot-glow damage flash, 0.0 - 1.0

//MARK: Rocket Constants
const Color ROCKET_OUTLINE_COLOR = {255, 255, 0, 255}; // Yellow outline for rockets.
const Color ROCKET_FILL_COLOR = {255, 255, 0, 40};
const Color ROCKET_OOB_OUTLINE_COLOR = {128, 128, 128, 0}; // Grey outline for rockets that are out-of-bounds.
const Color ROCKET_OOB_FILL_COLOR = {128, 128, 128, 0}; // Grey fill for rockets that are out-of-bounds.

const float ROCKET_RADIUS = 0.5f; // Radius of the rocket's collision box (a small sphere)

const float ROCKET_SPEED = 80.0f; // units/sec
const float ROCKET_KICKBACK_FACTOR = 0.2f; // Recoil applied to player on shoot, as a fraction of ROCKET_SPEED
const bool ROCKET_GRAVITY_ENABLED = false; // Per-rocket default: gravity affects the rocket. The OPTIONS "ROCKETS OBEY PHYSICS" toggle overrides this per match (see ROCKETS_OBEY_PHYSICS).
const bool ROCKET_VELOCITY_INHERITANCE_ENABLED = false; // Per-rocket default: rocket inherits the shooter's velocity at launch. Also driven by ROCKETS_OBEY_PHYSICS.
// OPTIONS "ROCKETS OBEY PHYSICS": one match-wide toggle that drives BOTH rocket
// gravity and shooter-velocity inheritance (input.h sets each fired rocket's
// gravityEnabled/velocityInheritance from GameSpace::rocketsObeyPhysics).
const bool ROCKETS_OBEY_PHYSICS = false; // default OFF: rockets fly straight, no inherited velocity (current behavior)
const float ROCKET_MUZZLE_CLEARANCE = 0.5f; // extra gap past (player radius + rocket radius) so a freshly-fired rocket clears the body, units
const float ROCKET_SPIN_SPEED = 9.0f; // how fast the star-polyhedron rocket spins about its travel axis, radians/sec (visual only)

//MARK: Explosion Constants
const int EXPLOSION_DAMAGE = 25;
const float EXPLOSION_DAMAGE_RADIUS = 25.0f; // units
const float EXPLOSION_MAX_RADIUS = EXPLOSION_DAMAGE_RADIUS * 1.8f; // units, visual radius of the explosion effect
const float EXPLOSION_EXPANSION_RATE = 15.0f; // How quickly the explosion expands, in units/sec
const float EXPLOSION_PUSHBACK_FACTOR = 1.0f; // fraction of damage applied as pushback force
// OPTIONS "FRIENDLY FIRE": when OFF, a player takes no splash DAMAGE from their own
// blast (self-knockback still applies, so rocket-jumping survives). Default ON keeps
// the current behavior. Consumed in ApplyExplosionSplashDamage (collisions.cpp).
const bool FRIENDLY_FIRE = true; // default ON: your own rocket can damage you (current behavior)

//MARK: Spark VFX Constants
// Sparks are pure visual particles (no collision). Shared physics/draw, spawned
// as one-time elimination bursts. (The jetpack exhaust plume that also used
// sparks was deprecated - see docs/exhaust-plume-archive.md.)
const float SPARK_GRAVITY = 6.0f;        // downward accel on sparks, units/s^2 (0 = none)
const float SPARK_DRAG = 1.5f;           // velocity damping per second
const float SPARK_STREAK_LENGTH = 0.6f;  // length of the drawn streak, units

// Elimination-burst spark lifetime (longer - debris lingers). Used by both the
// asteroid-destruction and player-elimination bursts.
const float SPARK_BURST_LIFETIME_MIN = 9.0f;
const float SPARK_BURST_LIFETIME_MAX = 18.0f;

// One-time spherical puff spawned when an asteroid OR a player is destroyed.
const int   ASTEROID_BURST_COUNT = 240;        // sparks per elimination burst (tunable quantity)
const float ASTEROID_BURST_SPEED_MIN = 12.0f;
const float ASTEROID_BURST_SPEED_MAX = 48.0f;

//MARK: ID-related Constants
const uint32_t ID_NONE = 0; // uninitialized/invalid sentinel

const uint32_t PLAYER_ID_BASE    = 0x00000001; // 1 - 255 (max 255 players)
const uint32_t NON_PLAYER_ID_BASE  = 0x00000100; // 256 - 65535

//MARK: Native client built-in server (address baked into the binary)
// Native client's built-in server. Empty host => no baked server: launching with
// no arg starts LOCAL single-player (dev default), and a URL arg goes networked.
// Set a host (e.g. a Vultr IP) to ship a binary that auto-connects: no-arg launch
// then connects over UDP and pivots to WebSocket if the UDP handshake stalls, and
// `./platformz local` is the single-player escape hatch. Override at build time:
//   -DPLATFORMZ_DEFAULT_SERVER_HOST='"203.0.113.10"'
#ifndef PLATFORMZ_DEFAULT_SERVER_HOST
#define PLATFORMZ_DEFAULT_SERVER_HOST ""       // e.g. "203.0.113.10"
#endif
#ifndef PLATFORMZ_DEFAULT_SERVER_PORT
#define PLATFORMZ_DEFAULT_SERVER_PORT "9000"
#endif
