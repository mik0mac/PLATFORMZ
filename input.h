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
inline PlayerInput PollLocalInput() {
    PlayerInput in;
    in.lookDelta = GetMouseDelta();
    if (IsKeyDown(KEY_W)) in.moveAxis.y += 1.0f;
    if (IsKeyDown(KEY_S)) in.moveAxis.y -= 1.0f;
    if (IsKeyDown(KEY_D)) in.moveAxis.x += 1.0f;
    if (IsKeyDown(KEY_A)) in.moveAxis.x -= 1.0f;
    in.jetpack      = IsKeyDown(KEY_SPACE);
    in.earthGravity = IsKeyDown(KEY_LEFT_SUPER); // = COMMAND.
    in.fire         = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    return in;
}

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
    if (in.fire && player.canShoot && player.isAlive) {
        player.shoot(); // ammo / canShoot bookkeeping (see Player::shoot)

        Rocket rocket;
        rocket.owner = &player; // track which player fired this rocket
        Vector3 eyePos = Vector3Add(player.position, Vector3{0, player.eyeHeight, 0});
        Vector3 aim = player.Forward();
        // Nudge the muzzle forward so the rocket clears the player and doesn't
        // detonate on whatever the player is standing on the instant it spawns.
        rocket.position  = Vector3Add(eyePos, Vector3Scale(aim, 1.0f));
        rocket.direction = aim;
        rocket.velocity  = Vector3Scale(aim, rocket.speed); // fire straight, no inherited velocity
        gameSpace.getRockets().push_back(rocket);

        // kickback (recoil)
        Vector3 kickback = Vector3Scale(aim, (-1 * rocket.kickback));
        player.velocity = Vector3Add(player.velocity, kickback);
    }
}
