#pragma once

//MARK: Physics Constants

const float MOON_GRAVITY = 1.62f; // moon gravity, m/s^2 (assuming 1 unit = 1 meter)
const float EARTH_GRAVITY = 9.81f; // earth gravity, m/s^2 

//MARK: Player Constants

const int PLAYER_MAX_AMMO = 100;
const int PLAYER_STARTING_AMMO = PLAYER_MAX_AMMO;
const int PLAYER_MAX_HEALTH = 100;
const int PLAYER_STARTING_HEALTH = PLAYER_MAX_HEALTH;
const float PLAYER_MAX_FUEL = 100.0f;
const float PLAYER_STARTING_FUEL = PLAYER_MAX_FUEL;
const float FUEL_CONSUMPTION_RATE = 5.0f; // Per sec.
const float FUEL_REGEN_RATE = 0.5f; // Per sec.

const int PLAYER_ELIMINATION_SCORE_VALUE = 100; // Points awarded to player for eliminating another player

//MARK: Asteroid Constants

const int ASTEROID_STARTING_HEALTH = 50;
const int ASTEROID_DAMAGE = 20;
const int ASTEROID_SCORE_VALUE = 100;
const int ASTEROID_FUEL_AWARD = 20;
const int ASTEROID_HEALTH_AWARD = 20;
const int ASTEROID_AMMO_AWARD = 20;

//MARK: Rocket Constants
const float ROCKET_SPEED = 40.0f; // units/sec
const float ROCKET_KICKBACK_FACTOR = 0.1f; // Recoil applied to player on shoot, as a fraction of ROCKET_SPEED


//MARK: Explosion Constants
const int EXPLOSION_DAMAGE = 25;
const float EXPLOSION_DAMAGE_RADIUS = 10.0f; // units
const float EXPLOSION_MAX_RADIUS = EXPLOSION_DAMAGE_RADIUS; // units, visual radius of the explosion effect
const float EXPLOSION_PUSHBACK_FACTOR = 1.0f; // fraction of damage applied as pushback force