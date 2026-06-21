#include "raylib.h"

int main() {
    // --- Setup (runs once) ---
    const int screenWidth = 1000;
    const int screenHeight = 700;
    InitWindow(screenWidth, screenHeight, "PLATFORMZ");
    SetTargetFPS(60);

    // Game state lives here, declared once, mutated every frame
    GameSpace gameSpace; // The main game space containing platforms, asteroids, and players
    gameSpace.generate(); // Generate the game space with platforms, asteroids, and players

    // --- The loop itself ---
    while (!WindowShouldClose()) {  // true when user hits X, presses Esc, etc.

        // 1. TIME
        // dt = seconds since last frame. Multiply all movement by this
        // so speed is consistent regardless of framerate.
        float dt = GetFrameTime();

        // 2. UPDATE
        // Read input, mutate game state. No drawing happens here.
        gameSpace.update(dt);

        // 3. DRAW
        // Everything between BeginDrawing/EndDrawing gets pushed to the screen.
        // No state should change in here - purely visual.
        BeginDrawing();
            ClearBackground(BLACK);
            gameSpace.draw();
            DrawFPS(10, 10);
        EndDrawing();

        // Loop repeats. raylib handles vsync/frame pacing via SetTargetFPS.
    }

    // --- Teardown (runs once, after the loop exits) ---
    CloseWindow();
    return 0;
}
