#pragma once

//MARK: Physics Constants
const float MOON_GRAVITY = 1.62f; // moon gravity, m/s^2 (assuming 1 unit = 1 meter)
const float EARTH_GRAVITY = 9.81f; // earth gravity, m/s^2

//MARK: GameSpace Constants
const float GAMESPACE_HALF_SIZE = 40.0f; // half-size of the game space cube, units.  Also used by Walls.
const int GAMESPACE_NUMBER_OF_PLATFORMS = 16; // Number of platforms in the game space
const int GAMESPACE_NUMBER_OF_ASTEROIDS = 8; // Number of asteroids in the game space
const int GAMESPACE_NUMBER_OF_PLAYERS = 2; // Number of players (index 0 is the local human; 1+ are wander-bots for testing)

//MARK: Wall Constants
const float WALL_ELASTICITY_PLAYER = 0.8f; // hit velocity is reflected and scaled by this (velocity = -velocity * elasticity)
const float WALL_ELASTICITY_ASTEROID = 1.01f; // hit velocity is reflected and scaled by this (velocity = -velocity * elasticity)
const int WALL_DAMAGE = 0; // damage dealt to the player on wall impact

//MARK: Platform Constants
const float PLATFORM_ELASTICITY_PLAYER = 0.25f; // For bouncy platforms, 0.0 - 1.0, determines how much the player bounces (velocity = -velocity * elasticity)
const float PLATFORM_ELASTICITY_ASTEROID = 1.0f; // For bouncy platforms, 0.0 - 1.0, determines how much the asteroid bounces (velocity = -velocity * elasticity)

const float PLATFORM_MIN_WIDTH = 12.0f; //GAMESPACE_HALF_SIZE / 12.0f; // 5.0f; // Minimum width of the platform
const float PLATFORM_MAX_WIDTH = 12.0f; //GAMESPACE_HALF_SIZE / 3.0f; // 20.0f; // Maximum width of the platform
const float PLATFORM_MIN_HEIGHT = 0.25f; // Minimum height of the platform
const float PLATFORM_MAX_HEIGHT = 0.25f; // Maximum height of the platform
const float PLATFORM_MIN_DEPTH = PLATFORM_MIN_WIDTH; // Minimum depth of the platform
const float PLATFORM_MAX_DEPTH = PLATFORM_MAX_WIDTH; // Maximum depth of the platform


//MARK: Player Shape / Size
// The player renders as a regular dodecahedron (DrawPlayer in shapes.h). Earlier
// prototype silhouettes (DART, DELTA, LANDER, POD) are archived in
// docs/player-shapes-archive.md.

// Jetpack exhaust: spawned spark particles streaming down out of the bottom
// while the jetpack fires (see SpawnSparkCone / main.cpp emission).
const float PLAYER_EXHAUST_RATE = 140.0f;     // sparks spawned per second while thrusting
const float PLAYER_EXHAUST_CONE = 0.35f;      // cone half-angle around straight-down, radians
const float PLAYER_EXHAUST_SPEED_MIN = 8.0f;  // spark ejection speed, units/sec
const float PLAYER_EXHAUST_SPEED_MAX = 16.0f;
const float PLAYER_EXHAUST_INHERIT = 0.3f;    // fraction of player velocity added to each spark

// The player is a sphere (rendered as a dodecahedron, DrawPlayer in shapes.h)
// centered on player.position; the camera sits at that center. PLAYER_SCALE
// tunes the size about the center point.
const float PLAYER_SCALE = 2.0f;         // proportional size multiplier
const float PLAYER_BASE_RADIUS = 1.0f;   // body/collider radius in units, before scale
const float PLAYER_RADIUS = PLAYER_BASE_RADIUS * PLAYER_SCALE; // == 2.0 (preserves current size)

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

//MARK: Player Constants
const float PLAYER_SPEED_WALK = 10.0f; // units/sec
const float PLAYER_ACCELERATION_WALK = 7.5f; // units/sec^2
const float PLAYER_SPEED_JETPACK = 16.0f; // units/sec
const float PLAYER_ACCELERATION_JETPACK = 12.0f; // units/sec^

const int PLAYER_MAX_AMMO = 100;
const int PLAYER_STARTING_AMMO = PLAYER_MAX_AMMO;
const int PLAYER_MAX_HEALTH = 100;
const int PLAYER_STARTING_HEALTH = PLAYER_MAX_HEALTH;
const float PLAYER_MAX_FUEL = 100.0f;
const float PLAYER_STARTING_FUEL = PLAYER_MAX_FUEL;

const float FUEL_CONSUMPTION_RATE = 5.0f; // Per sec.
const float FUEL_REGEN_RATE = 0.5f; // Per sec.
const float PLAYER_FIRE_RATE = 2.0f; // shots per second.

const int PLAYER_ELIMINATION_SCORE_AWARD = 100; // Points awarded to player for eliminating another player

//MARK: Bot Constants
// Test-only "wander" bots that drive non-local players (index 1+) through the
// same ApplyPlayerInput() path a human/network player uses (see input.h).
const float BOT_REROLL_MIN_SECONDS = 1.5f; // min time before a bot re-rolls its heading/movement
const float BOT_REROLL_MAX_SECONDS = 4.0f; // max time before a bot re-rolls its heading/movement
const float BOT_TURN_RATE = 2.0f; // max yaw the bot turns toward its target heading, radians/sec

const bool DIABLE_BOT_MOVEMENT = false; // If true, bots don't move or jetpack (for testing other features)

//MARK: Asteroid Constants
const int ASTEROID_STARTING_HEALTH = 25;
const int ASTEROID_DAMAGE = 20;
const int ASTEROID_SCORE_AWARD = 100;
const float ASTEROID_FUEL_AWARD = 20.0f;
const int ASTEROID_HEALTH_AWARD = 20;
const int ASTEROID_AMMO_AWARD = 20;

const float ASTEROID_MIN_SIZE = 4.0f; // Minimum radius of the asteroid
const float ASTEROID_MAX_SIZE = 4.0f; // GAMESPACE_HALF_SIZE / 5.0f; // Maximum radius of the asteroid
const float ASTEROID_MIN_SPEED = 6.0f; // Minimum speed of the asteroid
const float ASTEROID_MAX_SPEED = 12.0f; // GAMESPACE_HALF_SIZE / 3.0f; // Maximum speed of the asteroid

const float ASTEROID_FLASH_DURATION = 4.0f; // length of the hot-glow damage flash, seconds
const float ASTEROID_FLASH_INTENSITY = 0.6f; // intensity of the hot-glow damage flash, 0.0 - 1.0

//MARK: Rocket Constants
const float ROCKET_SPEED = 60.0f; // units/sec
const float ROCKET_KICKBACK_FACTOR = 0.1f; // Recoil applied to player on shoot, as a fraction of ROCKET_SPEED
const bool ROCKET_GRAVITY_ENABLED = true; // If true, rockets are affected by gravity.
const bool ROCKET_VELOCITY_INHERITANCE_ENABLED = true; // If true, rockets inherit the player's velocity when fired.
const float ROCKET_MUZZLE_CLEARANCE = 0.5f; // extra gap past (player radius + rocket radius) so a freshly-fired rocket clears the body, units
const float ROCKET_SPIN_SPEED = 18.0f; // how fast the star-polyhedron rocket spins about its travel axis, radians/sec (visual only)

//MARK: Explosion Constants
const int EXPLOSION_DAMAGE = 25;
const float EXPLOSION_DAMAGE_RADIUS = 25.0f; // units
const float EXPLOSION_MAX_RADIUS = EXPLOSION_DAMAGE_RADIUS * 1.8f; // units, visual radius of the explosion effect
const float EXPLOSION_EXPANSION_RATE = 15.0f; // How quickly the explosion expands, in units/sec
const float EXPLOSION_PUSHBACK_FACTOR = 1.0f; // fraction of damage applied as pushback force

//MARK: Spark VFX Constants
// Sparks are pure visual particles (no collision). Shared physics/draw; lifetime
// is split so jetpack exhaust and elimination bursts tune independently.
const float SPARK_GRAVITY = 6.0f;        // downward accel on sparks, units/s^2 (0 = none)
const float SPARK_DRAG = 1.5f;           // velocity damping per second
const float SPARK_STREAK_LENGTH = 0.6f;  // length of the drawn streak, units

// Jetpack exhaust spark lifetime (short - sparks fade fast behind the player).
const float PLAYER_EXHAUST_LIFETIME_MIN = 0.30f;
const float PLAYER_EXHAUST_LIFETIME_MAX = 0.70f;

// Elimination-burst spark lifetime (longer - debris lingers). Used by both the
// asteroid-destruction and player-elimination bursts.
const float SPARK_BURST_LIFETIME_MIN = 9.0f;
const float SPARK_BURST_LIFETIME_MAX = 18.0f;

// One-time spherical puff spawned when an asteroid OR a player is destroyed.
const int   ASTEROID_BURST_COUNT = 240;        // sparks per elimination burst (tunable quantity)
const float ASTEROID_BURST_SPEED_MIN = 12.0f;
const float ASTEROID_BURST_SPEED_MAX = 48.0f;