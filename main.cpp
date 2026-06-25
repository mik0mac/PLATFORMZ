#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include "gamespace.h"
#include "shapes.h"
#include "collisions.h"
#include "camera.h"
#include "input.h"


int main() {
//MARK: SETUP
    // --- Setup (runs once) ---
    const int screenWidth = 1000;
    const int screenHeight = 700;
    const int textHeight = 20;
    InitWindow(screenWidth, screenHeight, "PLATFORMZ");
    SetTargetFPS(60);
    DisableCursor(); // captures mouse for free-look, like a standard 3D game

    // The 3D scene is rendered into this off-screen target each frame so the
    // finished image can be sampled and distorted on the way to the screen -
    // that's what drives the damage glitch (a draw-on-top overlay can't
    // scramble the pixels that are already there).
    RenderTexture2D sceneTarget = LoadRenderTexture(screenWidth, screenHeight);

    // Greyscale post-process, used on death. Fragment-only shader (the 0 vertex
    // arg means "use raylib's default vertex shader"); kept inline so there's no
    // extra file to ship. #version 330 = OpenGL 3.3, raylib's desktop context.
    // colDiffuse is applied AFTER the luminance, so any draw tint still shows
    // through (e.g. the glitch's red/blue split copies keep a faint fringe).
    const char* grayscaleFrag =
        "#version 330\n"
        "in vec2 fragTexCoord;\n"
        "in vec4 fragColor;\n"
        "uniform sampler2D texture0;\n"
        "uniform vec4 colDiffuse;\n"
        "out vec4 finalColor;\n"
        "void main() {\n"
        "    vec4 texel = texture(texture0, fragTexCoord);\n"
        "    float lum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));\n" // Rec.601 luma
        "    finalColor = vec4(vec3(lum), texel.a) * colDiffuse * fragColor;\n"
        "}\n";
    Shader grayscaleShader = LoadShaderFromMemory(0, grayscaleFrag);

    // Game state lives here, declared once, mutated every frame
    GameSpace gameSpace; // The main game space containing platforms, asteroids, and players
    gameSpace.generate(); // Generate the game space with platforms, asteroids, and players

    CollisionGrid collisionGrid; // Spatial grid, rebuilt each frame in RunCollisionChecks

    // One wander-bot per non-local player (players[1..]). Index 0 is the local
    // human. The players vector isn't resized during the loop, so these stay
    // 1:1 with players[i+1]. Empty when there's only one player (bots disabled).
    std::vector<BotState> botStates(gameSpace.getPlayers().size() - 1);
//MARK: MAIN LOOP
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

        // Gather this frame's intent into a source-agnostic struct, then apply
        // it. For a networked player the same struct would arrive over the wire
        // and feed the identical ApplyPlayerInput() path (see input.h).
        PlayerInput in = PollLocalInput();
        float gravity = in.earthGravity ? EARTH_GRAVITY : MOON_GRAVITY; // constants stay here
        ApplyPlayerInput(player, in, dt, gravity, gameSpace);

        // Drive the test bots (players[1..]) through the same input path with
        // randomly-generated wander input. Bots always use moon gravity - they
        // never hold the earth-gravity key.
        std::vector<Player>& players = gameSpace.getPlayers();
        for (int i = 1; i < (int)players.size(); ++i) {
            PlayerInput botIn = MakeBotInput(players[i], botStates[i - 1], dt);
            ApplyPlayerInput(players[i], botIn, dt, MOON_GRAVITY, gameSpace);
        }

        // Jetpack exhaust: spawn spark particles streaming down out of each
        // thrusting player's bottom. Visual only - emitted here (game-state
        // phase) so velocity/jetpack state is current; ticked in updatePositions.
        for (Player& pl : players) {
            if (!(pl.isUsingJetpack && pl.hasFuel())) continue;
            float want = PLAYER_EXHAUST_RATE * dt;          // fractional count -> integer with random carry
            int n = (int)want;
            if (RandomFloat(0.0f, 1.0f) < (want - (float)n)) n++;
            if (n <= 0) continue;
            Vector3 origin = pl.position;
            origin.y -= 0.5f * pl.size.y;                   // bottom of the body
            SpawnSparkCone(gameSpace.getSparks(), origin, {0, -1, 0}, PLAYER_EXHAUST_CONE,
                           PLAYER_EXHAUST_SPEED_MIN, PLAYER_EXHAUST_SPEED_MAX, n,
                           Color{255, 200, 80, 255},
                           PLAYER_EXHAUST_LIFETIME_MIN, PLAYER_EXHAUST_LIFETIME_MAX,
                           Vector3Scale(pl.velocity, PLAYER_EXHAUST_INHERIT));
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
//MARK:DRAW
        // 3. DRAW
        // Two passes: first render the 3D world into sceneTarget (capture),
        // then composite that texture to the screen - cleanly, or scrambled
        // by the damage glitch. No game state changes in here - purely visual.

        // Capture pass. Must be its own BeginTextureMode block, outside
        // BeginDrawing - you can't nest the two, and the frame has to exist
        // before it can be distorted. gameSpace.draw() is unchanged; it just
        // renders into the target now instead of the back buffer.
        BeginTextureMode(sceneTarget);
            ClearBackground(BLACK);
            BeginMode3D(CameraFromPlayer(player, player.eyeHeight));
                gameSpace.draw(0); // skip drawing the local player (index 0)
            EndMode3D();
        EndTextureMode();

        // Screen pass. Everything between BeginDrawing/EndDrawing hits the screen.
        BeginDrawing();
            ClearBackground(BLACK);

            float hurt = player.flashIntensity(); // 1.0 just after a hit -> 0.0
            Texture2D tex = sceneTarget.texture;
            // Render textures are stored Y-flipped, so every source rect that
            // samples this texture uses a NEGATIVE height to flip it upright.

            // On death: drive the glitch with a sustained intensity (flashIntensity
            // would otherwise decay to 0 and the effect would fade out), and render
            // the whole blit through the greyscale shader below.
            const float DEATH_GLITCH = 0.5f;
            if (!player.isAlive) hurt = DEATH_GLITCH;

            if (!player.isAlive) BeginShaderMode(grayscaleShader);
            if (hurt <= 0.0f) {
                // Clean path: one flipped full-screen blit.
                DrawTexturePro(tex, {0, 0, (float)tex.width, -(float)tex.height},
                    {0, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0.0f, WHITE);
            } else {
                // (a) Horizontal tearing: redraw the frame in bands, each band
                //     shoved sideways. Magnitude scales with hurt, so it eases
                //     back to a still image as the flash resolves.
                const int bandH = 14;
                float maxShift = 40.0f * hurt;
                for (int y = 0; y < screenHeight; y += bandH) {
                    // jitter only some bands so the tearing reads as intermittent
                    float shift = (RandomFloat(0, 1) < 0.5f) ? 0.0f : RandomFloat(-maxShift, maxShift);
                    int h = (y + bandH > screenHeight) ? (screenHeight - y) : bandH;
                    Rectangle src{0, (float)(tex.height - y), (float)tex.width, -(float)h};
                    Rectangle dst{shift, (float)y, (float)screenWidth, (float)h};
                    DrawTexturePro(tex, src, dst, {0, 0}, 0.0f, WHITE);
                }
                // (b) RGB split: additive red/blue copies nudged left/right for
                //     a chromatic-aberration fringe.
                float ca = 6.0f * hurt;
                Rectangle fullSrc{0, 0, (float)tex.width, -(float)tex.height};
                BeginBlendMode(BLEND_ADDITIVE);
                    DrawTexturePro(tex, fullSrc, {-ca, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0.0f, {255, 0, 0, 160});
                    DrawTexturePro(tex, fullSrc, { ca, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0.0f, {0, 80, 255, 160});
                EndBlendMode();
                // (c) Static: a few random bright scanline streaks.
                int streaks = (int)(8 * hurt);
                for (int i = 0; i < streaks; ++i) {
                    int sy = (int)RandomFloat(0, screenHeight);
                    int sh = (int)RandomFloat(1, 3);
                    unsigned char a = (unsigned char)(RandomFloat(20, 90) * hurt);
                    DrawRectangle(0, sy, screenWidth, sh, {255, 255, 255, a});
                }
            }
            if (!player.isAlive) EndShaderMode();
// MARK: HUD
            // text size, x, y, color.
            // draw onscreen text HUD.
            if (!player.isAlive) {
                // death centered on whole screen, not just the top-left corner like the rest of the HUD
                DrawText("YOU HAVE BEEN ELIMINATED. BETTER LUCK NEXT TIME!", 56, 700, 300, RED);
            }
            else {
                // DrawFPS(10, 10); // Draws the current FPS in the top-left corner of the screen
                // DrawText("WASD move | mouse look | Click fire | Space/Ctrl jetpack up-down | Esc toggle cursor", 10, 30, 14, DARKGRAY);
                DrawText(TextFormat("Rockets: %d", player.ammo), 10, textHeight * 1, 14, YELLOW);
                DrawText(TextFormat("Fuel: %.1f", player.fuel), 10, textHeight * 2, 14, GREEN);
                DrawText(TextFormat("Health: %d", player.health), 10, textHeight * 3, 14, RED);
                if (in.earthGravity) {
                DrawText("EARTH GRAVITY ENGAGED!!!", 10, textHeight * 4, 14, BLUE);
            }
        }
            
        EndDrawing();

        // Loop repeats. raylib handles vsync/frame pacing via SetTargetFPS.
    }

    // --- Teardown (runs once, after the loop exits) ---
    UnloadRenderTexture(sceneTarget);
    UnloadShader(grayscaleShader);
    CloseWindow();
    return 0;
}
