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

    // Game state lives here, declared once, mutated every frame
    GameSpace gameSpace; // The main game space containing platforms, asteroids, and players
    gameSpace.generate(); // Generate the game space with platforms, asteroids, and players

    CollisionGrid collisionGrid; // Spatial grid, rebuilt each frame in RunCollisionChecks
    FlyCam flyCam; // Free-fly camera, not part of GameSpace - see gamespace.h::draw() note

    // --- The loop itself ---
    while (!WindowShouldClose()) {  // true when user hits X, presses Esc, etc.

        // 1. TIME
        // dt = seconds since last frame. Multiply all movement by this
        // so speed is consistent regardless of framerate.
        float dt = GetFrameTime();

        // 2. UPDATE
        // Read input, mutate game state. No drawing happens here.
        flyCam.Update(dt);
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

            BeginMode3D(flyCam.ToCamera3D());
                gameSpace.draw();
            EndMode3D();

            DrawFPS(10, 10);
            DrawText("WASD move | mouse look | Space/Ctrl up-down | Shift sprint | Esc toggle cursor", 10, 30, 14, DARKGRAY);
        EndDrawing();

        // Loop repeats. raylib handles vsync/frame pacing via SetTargetFPS.
    }

    // --- Teardown (runs once, after the loop exits) ---
    CloseWindow();
    return 0;
}
