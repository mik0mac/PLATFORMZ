#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include "gamespace.h"
#include "shapes.h"
#include "collisions.h"
#include "camera.h"

int main() {
    // --- Setup (runs once) ---
    const int screenWidth = 1000;
    const int screenHeight = 700;
    InitWindow(screenWidth, screenHeight, "PLATFORMZ");
    SetTargetFPS(60);
    DisableCursor(); // captures mouse for free-look, like a standard 3D game

    const float eyeHeight = 1.6f; // meters above player.position - roughly eye level within the 2.0-tall collision box

    // Game state lives here, declared once, mutated every frame
    GameSpace gameSpace; // The main game space containing platforms, asteroids, and players
    gameSpace.generate(); // Generate the game space with platforms, asteroids, and players

    CollisionGrid collisionGrid; // Spatial grid, rebuilt each frame in RunCollisionChecks

    // --- The loop itself ---
    while (!WindowShouldClose()) {  // true when user hits X, presses Esc, etc.

        // 1. TIME
        // dt = seconds since last frame. Multiply all movement by this
        // so speed is consistent regardless of framerate.
        float dt = GetFrameTime();

        // 2. UPDATE
        // Read input, mutate game state. No drawing happens here.
        // Player is the source of truth for movement/look - the camera is
        // synced FROM the player after this, never the other way around.
        Player& player = gameSpace.getPlayers()[0]; // single-player for now

        Vector2 mouseDelta = GetMouseDelta();
        player.updateLook(mouseDelta);

        Vector2 moveInput{0, 0}; // x = strafe, y = forward/back
        if (IsKeyDown(KEY_W)) moveInput.y += 1.0f;
        if (IsKeyDown(KEY_S)) moveInput.y -= 1.0f;
        if (IsKeyDown(KEY_D)) moveInput.x += 1.0f;
        if (IsKeyDown(KEY_A)) moveInput.x -= 1.0f;

        player.isUsingJetpack = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_LEFT_CONTROL);
        bool jetpackUp = IsKeyDown(KEY_SPACE);
        bool jetpackDown = IsKeyDown(KEY_LEFT_CONTROL);
        player.updateVelocity(dt, moveInput, jetpackUp, jetpackDown);
        player.updateFuel(dt, player.isUsingJetpack);

        // Fire a rocket on left-click. IsMouseButtonPressed fires once per
        // click, so this is naturally single-shot - no cooldown needed.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && player.canShoot) {
            player.shoot(); // ammo / canShoot bookkeeping (see Player::shoot)

            Rocket rocket;
            Vector3 eyePos = Vector3Add(player.position, Vector3{0, eyeHeight, 0});
            Vector3 aim = player.Forward();
            // Nudge the muzzle forward so the rocket clears the player and
            // doesn't detonate on whatever the player is standing on the
            // instant it spawns.
            rocket.position = Vector3Add(eyePos, Vector3Scale(aim, 1.0f));
            rocket.direction = aim;
            rocket.velocity = Vector3Scale(aim, rocket.speed); // fire straight, no inherited velocity
            gameSpace.getRockets().push_back(rocket);
        }

        gameSpace.updatePositions(dt);

        // Collision detection and response:
        RunCollisionChecks(gameSpace, collisionGrid);

        // Remove destroyed asteroids and rockets, finished explosions, etc.
        gameSpace.updateActiveObjects(); 

        if (IsKeyPressed(KEY_ESCAPE)) {
            // toggle cursor capture so you can alt-tab / quit comfortably
            if (IsCursorHidden()) EnableCursor(); else DisableCursor();
        }

        // 3. DRAW
        // Everything between BeginDrawing/EndDrawing gets pushed to the screen.
        // No state should change in here - purely visual.
        BeginDrawing();
            ClearBackground(BLACK);

            BeginMode3D(CameraFromPlayer(player, eyeHeight));
                gameSpace.draw();
            EndMode3D();

            DrawFPS(10, 10);
            DrawText("WASD move | mouse look | Click fire | Space/Ctrl jetpack up-down | Esc toggle cursor", 10, 30, 14, DARKGRAY);
            DrawText(TextFormat("Ammo: %d", player.ammo), 10, 50, 14, DARKGRAY);
        EndDrawing();

        // Loop repeats. raylib handles vsync/frame pacing via SetTargetFPS.
    }

    // --- Teardown (runs once, after the loop exits) ---
    CloseWindow();
    return 0;
}
