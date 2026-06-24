#pragma once

//MARK: Physics Constants
const float MOON_GRAVITY = 1.62f; // moon gravity, m/s^2 (assuming 1 unit = 1 meter)
const float EARTH_GRAVITY = 9.81f; // earth gravity, m/s^2

//MARK: GameSpace Constants
const float GAMESPACE_HALF_SIZE = 80.0f; // half-size of the game space cube, units.  Also used by Walls.
const int GAMESPACE_NUMBER_OF_PLATFORMS = 24; // Number of platforms in the game space
const int GAMESPACE_NUMBER_OF_ASTEROIDS = 12; // Number of asteroids in the game space
const int GAMESPACE_NUMBER_OF_PLAYERS = 2; // Number of players (index 0 is the local human; 1+ are wander-bots for testing)

//MARK: Wall Constants
const float WALL_ELASTICITY_PLAYER = 0.8f; // hit velocity is reflected and scaled by this (velocity = -velocity * elasticity)
const float WALL_ELASTICITY_ASTEROID = 1.01f; // hit velocity is reflected and scaled by this (velocity = -velocity * elasticity)
const int WALL_DAMAGE = 0; // damage dealt to the player on wall impact

//MARK: Platform Constants
const float PLATFORM_ELASTICITY_PLAYER = 0.25f; // For bouncy platforms, 0.0 - 1.0, determines how much the player bounces (velocity = -velocity * elasticity)
const float PLATFORM_ELASTICITY_ASTEROID = 1.0f; // For bouncy platforms, 0.0 - 1.0, determines how much the asteroid bounces (velocity = -velocity * elasticity)

const float PLATFORM_MIN_WIDTH = GAMESPACE_HALF_SIZE / 8.0f; // 5.0f; // Minimum width of the platform
const float PLATFORM_MAX_WIDTH = GAMESPACE_HALF_SIZE / 2.0f; // 20.0f; // Maximum width of the platform
const float PLATFORM_MIN_HEIGHT = 0.5f; // Minimum height of the platform
const float PLATFORM_MAX_HEIGHT = 0.5f; // Maximum height of the platform
const float PLATFORM_MIN_DEPTH = PLATFORM_MIN_WIDTH; // Minimum depth of the platform
const float PLATFORM_MAX_DEPTH = PLATFORM_MAX_WIDTH; // Maximum depth of the platform


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

//MARK: Asteroid Constants
const int ASTEROID_STARTING_HEALTH = 50;
const int ASTEROID_DAMAGE = 20;
const int ASTEROID_SCORE_AWARD = 100;
const float ASTEROID_FUEL_AWARD = 20.0f;
const int ASTEROID_HEALTH_AWARD = 20;
const int ASTEROID_AMMO_AWARD = 20;

const float ASTEROID_MIN_SIZE = 8.0f; // Minimum radius of the asteroid
const float ASTEROID_MAX_SIZE = 8.0f; // GAMESPACE_HALF_SIZE / 5.0f; // Maximum radius of the asteroid
const float ASTEROID_MIN_SPEED = 8.0f; // Minimum speed of the asteroid
const float ASTEROID_MAX_SPEED = 16.0f; // GAMESPACE_HALF_SIZE / 3.0f; // Maximum speed of the asteroid

const float ASTEROID_FLASH_DURATION = 4.0f; // length of the hot-glow damage flash, seconds
const float ASTEROID_FLASH_INTENSITY = 0.6f; // intensity of the hot-glow damage flash, 0.0 - 1.0

//MARK: Rocket Constants
const float ROCKET_SPEED = 60.0f; // units/sec
const float ROCKET_KICKBACK_FACTOR = 0.1f; // Recoil applied to player on shoot, as a fraction of ROCKET_SPEED
const bool ROCKET_GRAVITY_ENABLED = true; // If true, rockets are affected by gravity.
const bool ROCKET_VELOCITY_INHERITANCE_ENABLED = true; // If true, rockets inherit the player's velocity when fired.

//MARK: Explosion Constants
const int EXPLOSION_DAMAGE = 25;
const float EXPLOSION_DAMAGE_RADIUS = 25.0f; // units
const float EXPLOSION_MAX_RADIUS = EXPLOSION_DAMAGE_RADIUS * 1.8f; // units, visual radius of the explosion effect
const float EXPLOSION_EXPANSION_RATE = 15.0f; // How quickly the explosion expands, in units/sec
const float EXPLOSION_PUSHBACK_FACTOR = 1.0f; // fraction of damage applied as pushback force