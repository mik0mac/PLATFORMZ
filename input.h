// Input decoupling: turn raw devices (or, later, network messages) into a plain
// PlayerInput struct, then apply that struct to a Player. Gathering and applying
// are split on purpose - PollLocalInput() is the only place that touches raylib
// input for gameplay, while ApplyPlayerInput() is source-agnostic, so a remote
// player can be driven through the exact same path with a deserialized struct.
// Input/render-adjacent free functions, kept out of main.cpp and the game-logic
// headers - same spirit as CameraFromPlayer in camera.h.

#pragma once

#include "raylib.h"
#include "raymath.h"

#include "gamespace.h" // GameSpace, Player, Rocket
#include "random.h"    // RandomFloat
#include "constants.h" // BOT_* tuning

//MARK: PlayerInput
// Per-frame intent for ONE player, decoupled from its source. Filled locally by
// PollLocalInput(); over a network the same struct is deserialized from a remote
// client and applied through the identical ApplyPlayerInput() path.
struct PlayerInput {
    Vector2 lookDelta{0, 0};   // mouse delta: x = yaw, y = pitch
    Vector2 moveAxis{0, 0};    // x = strafe (+right), y = forward/back (+fwd)
    bool jetpack = false;      // holding jump/up
    bool earthGravity = false; // holding the stronger-gravity key
    bool fire = false;         // fire pressed THIS frame (single-shot)
};

//MARK: Gather
// Sample the local input devices into a PlayerInput. Swap this out (or feed a
// deserialized struct instead) to drive a player from the network.
// This is the only function here that touches raylib input, so it's excluded
// from the headless server build (PLATFORMZ_SERVER) - the server feeds players
// deserialized PlayerInput structs through ApplyPlayerInput() instead.
#ifndef PLATFORMZ_SERVER
inline PlayerInput PollLocalInput() {
    PlayerInput in;
    in.lookDelta = GetMouseDelta();
    if (IsKeyDown(KEY_W)) in.moveAxis.y += 1.0f;
    if (IsKeyDown(KEY_S)) in.moveAxis.y -= 1.0f;
    if (IsKeyDown(KEY_D)) in.moveAxis.x += 1.0f;
    if (IsKeyDown(KEY_A)) in.moveAxis.x -= 1.0f;
    in.jetpack      = IsKeyDown(KEY_SPACE);
    in.earthGravity = IsKeyDown(KEY_LEFT_SHIFT); // hold for stronger (earth) gravity
    in.fire         = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    return in;
}
#endif // PLATFORMZ_SERVER

//MARK: Apply
// Apply one player's intent: look, movement/fuel, and firing. `gravity` is
// passed in (resolved by the caller) so the gravity constants stay in main.cpp
// per the repo convention. Firing spawns a rocket into GameSpace, hence the
// param. Same calls/order as the logic this replaced in main.cpp.
inline void ApplyPlayerInput(Player& player, const PlayerInput& in,
                             float dt, float gravity, GameSpace& gameSpace) {
    player.updateLook(in.lookDelta);
    player.isUsingJetpack = in.jetpack;
    player.updateVelocity(dt, in.moveAxis, gravity);
    player.updateFuel(dt, player.isUsingJetpack);

    // IsMouseButtonPressed already fires once per click, so this stays
    // naturally single-shot - no cooldown needed.
    if (in.fire) {
        bool shotFired = player.shoot(); // ammo / canShoot bookkeeping (see Player::shoot)
        if (shotFired) {
            // MARK: Rocket spawn
            Rocket rocket;
            rocket.id = (player.id << 24) | (player.rocketCounter++);

            rocket.owner = &player; // track which player fired this rocket
            Vector3 eyePos = player.position; // eye is at the sphere center
            Vector3 aim = player.Forward();
            // Push the muzzle past the body surface so the rocket spawns fully
            // clear of the player: player radius + rocket radius + a small gap.
            // Tied to player.radius so it scales with PLAYER_SCALE.
            float muzzleOffset = player.radius + rocket.size + ROCKET_MUZZLE_CLEARANCE;
            rocket.position  = Vector3Add(eyePos, Vector3Scale(aim, muzzleOffset));
            // Seed the swept-collision start at the eye, not the muzzle, so the
            // first segment covers the eye -> muzzle gap. A platform the player
            // is standing on straddles that gap on a downward shot.
            rocket.prevPosition = eyePos;
            rocket.direction = aim;
            rocket.velocity  = Vector3Scale(aim, rocket.speed); // fire straight, no inherited velocity
            if (rocket.velocityInheritance) {
                rocket.velocity = Vector3Add(rocket.velocity, player.velocity); // inherit player's velocity
            }
            gameSpace.getRockets().push_back(rocket);
            gameSpace.emitAudio(FX_ROCKET_LAUNCH, rocket.position, player.id);

            // kickback (recoil)
            Vector3 kickback = Vector3Scale(aim, (-1 * rocket.kickback));
            player.velocity = Vector3Add(player.velocity, kickback);
        }
        else {
            // no ammo, play a SFX.
            gameSpace.emitAudio(FX_NO_AMMO, player.position, player.id);
        }
    }
}

//MARK: Bot input
// Test-only AI input source. Drives a non-local player by producing the same
// PlayerInput a human would, so it flows through the identical ApplyPlayerInput()
// path above. PlayerInput is per-frame/stateless, so the persistent wander
// state (heading, held movement, re-roll timer) lives here in BotState - one
// per bot, owned by the caller (main.cpp).
struct BotState {
    float timer = 0.0f;       // seconds until the next re-roll; <=0 forces an immediate re-roll
    float targetYaw = 0.0f;   // heading the bot is steering toward (radians)
    Vector2 moveAxis{0, 0};   // currently-held move intent (x = strafe, y = fwd/back)
    bool jetpack = false;     // currently-held jetpack intent
};

// Produce one frame of wander input for a bot. Re-rolls a new heading/movement
// on a random timer; between re-rolls it steers smoothly toward targetYaw and
// holds the chosen movement. Never fires (movement-only test bot).
inline PlayerInput MakeBotInput(const Player& bot, BotState& state, float dt) {
    state.timer -= dt;
    if (state.timer <= 0.0f) {
        state.targetYaw = RandomFloat(-PI, PI);
        // bias toward forward travel so the bot roams rather than shuffling
        state.moveAxis = { RandomFloat(-0.5f, 0.5f), RandomFloat(0.25f, 1.0f) };
        state.jetpack = RandomFloat(0.0f, 1.0f) < 0.5f;
        state.timer = RandomFloat(BOT_REROLL_MIN_SECONDS, BOT_REROLL_MAX_SECONDS);
    }

    PlayerInput in;
    in.moveAxis = state.moveAxis;
    in.jetpack = state.jetpack;

    if (DISABLE_BOT_MOVEMENT) {
        in.moveAxis = {0, 0};
        in.jetpack = false;
    }

    // Steer toward targetYaw, turning at most BOT_TURN_RATE this frame. Wrap the
    // error into [-PI, PI] so the bot always turns the short way around.
    float yawError = state.targetYaw - bot.yaw;
    while (yawError > PI) yawError -= 2.0f * PI;
    while (yawError < -PI) yawError += 2.0f * PI;
    float maxStep = BOT_TURN_RATE * dt;
    float yawStep = Clamp(yawError, -maxStep, maxStep);
    // updateLook() scales lookDelta by lookSensitivity, so pre-divide to land on
    // the yaw step we actually want (feeding the bot through the human path).
    in.lookDelta.x = yawStep / bot.lookSensitivity;

    return in;
}
