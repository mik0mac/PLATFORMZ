#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include "gamespace.h"
#include "shapes.h"
#include "collisions.h"
#include "camera.h"
#include "input.h"
#include "audio.h"     // sound FX que, load/unload, trigger
#include "net_client.h" // multiplayer: WebSocket client
#include "wire.h"       // multiplayer: input serialize / state apply

#include <string>
#include <unordered_map>
#include <unordered_set>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h> // emscripten_run_script_string (read page URL)
#endif


// PLATFORMZ runs in one of two modes:
//   ./platformz                      -> local single-player (host the sim here)
//   ./platformz ws://host:9000       -> networked client of an authoritative
//   ./platformz wss://host/...          server (send input, render its state)
// The networked path reuses the exact same headers; it just doesn't run the
// sim locally - the server owns it (see server/server_main.cpp).
//
// In the browser there's no command line, so the web build is always a
// networked client and the server URL comes from the page query string.
int main(int argc, char** argv) {
#if defined(__EMSCRIPTEN__)
    (void)argc; (void)argv;
    const bool networked = true;
    // e.g. platformz.html?server=ws://192.168.1.20:9000
    // Falls back to the page's own host on :9000 if the param is absent.
    const std::string serverUrl = [] {
        const char* s = emscripten_run_script_string(
            "(new URLSearchParams(location.search).get('server')) || "
            "('ws://' + location.hostname + ':9000')");
        return std::string(s ? s : "");
    }();
#else
    const bool networked = (argc > 1);
    const std::string serverUrl = networked ? argv[1] : std::string();
#endif
//MARK: SETUP
    // --- Setup (runs once) ---
    const int screenWidth = 1000;
    const int screenHeight = 700;
    const int textHeight = 20;
    InitWindow(screenWidth, screenHeight, "PLATFORMZ");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL); // Esc is ours (free/recapture the mouse), not raylib's
                          // quit key - quit via the window close button or Cmd+Q.
    DisableCursor(); // captures mouse for free-look, like a standard 3D game

    // MARK: AUDIO
    // raylib's mixer must be up before any sound loads. The fxTable maps each
    // AudioFXId (the shared client/server wire id) to a loaded sound; game
    // events arrive as ids (locally or over the network) and index into it.
    InitAudioDevice();
    SetMasterVolume(1.0f);
    audioFX fxTable[FX_COUNT] = {
        audioFX("assets/sounds/rocket_launch.wav",  1.0f), // FX_ROCKET_LAUNCH
        audioFX("assets/sounds/explosion.wav",      1.0f), // FX_EXPLOSION
        audioFX("assets/sounds/asteroid_break.wav", 1.0f), // FX_ASTEROID_BREAK
        audioFX("assets/sounds/player_hit.wav",     1.0f), // FX_PLAYER_HIT
        audioFX("assets/sounds/player_death.wav",   1.0f), // FX_PLAYER_DEATH
    };
    for (audioFX& fx : fxTable) fx.load();

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

    CollisionGrid collisionGrid; // Spatial grid, rebuilt each frame in RunCollisionChecks (local mode only)

    AudioQueue audioQueue; // queue of sound events to play after all state updates.

    // --- Networking (networked mode only) ---
    NetClient net;
    int       myIndex   = -1;     // our player slot, from the server's welcome packet
    uint32_t  inputSeq  = 0;      // monotonically increasing input sequence number
    float     predYaw   = 0.0f;   // locally-predicted look (mouse drives this every
    float     predPitch = 0.0f;   // frame so aiming is instant, not server-round-trip)
    bool      predInit  = false;  // seed predYaw/predPitch from the server spawn once
    int       prevHealth = -1;    // last frame's health, to detect a server-side hit
    float     netHurt   = 0.0f;   // client hit-flash intensity (the server owns damage,
                                  // so flashIntensity() is always 0 here - drive it locally)

    // One wander-bot per non-local player (players[1..]) - LOCAL MODE ONLY.
    // Index 0 is the local human. In networked mode there are no bots; empty
    // slots simply stay unoccupied until another client connects.
    std::vector<BotState> botStates;

    // MARK: SCREEN STATE
    // The loop runs in one of three screens. TITLE and GAME_OVER are simple
    // placeholder overlays handled at the top of the loop; PLAYING is the
    // original game body (update -> draw). Play is gated by TITLE and ends back
    // there via GAME_OVER, so the world is set up on each PLAYING entry (not
    // before the loop) and torn down on the way back to the title.
    enum class GameScreen { TITLE, PLAYING, GAME_OVER };
    GameScreen screen = GameScreen::TITLE;

    // TITLE -> PLAYING: stand up a fresh run. Local hosts the sim; networked
    // kicks off the connection (the connecting-screen guard below covers the
    // wait) and resets the per-session prediction/flash state.
    auto startGame = [&]() {
        if (networked) {
            net.connect(serverUrl); // non-blocking; IXWebSocket retries on its own thread
            TraceLog(LOG_INFO, "Networked mode: connecting to %s", serverUrl.c_str());
            myIndex = -1; inputSeq = 0; predInit = false; prevHealth = -1; netHurt = 0.0f;
        } else {
            gameSpace.generate(); // platforms, asteroids, and player slots
            botStates.assign(gameSpace.getPlayers().size() - 1, BotState{});
        }
        screen = GameScreen::PLAYING;
    };

    // GAME_OVER -> TITLE: drop the connection (networked) and wipe the world so
    // the next run starts clean.
    auto returnToTitle = [&]() {
        if (networked) net.stop();
        gameSpace.clear();
        screen = GameScreen::TITLE;
    };

    // Centered placeholder text helper (screenWidth is in scope).
    auto DrawCentered = [&](const char* t, int y, int size, Color c) {
        DrawText(t, (screenWidth - MeasureText(t, size)) / 2, y, size, c);
    };

    // "Press any key to start/continue": any key other than Escape (which is
    // reserved for the cursor toggle), or a left-mouse click.
    auto startPressed = [&]() {
        int k = GetKeyPressed();
        return (k != 0 && k != KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    };
//MARK: MAIN LOOP
    // --- The loop itself ---
    while (!WindowShouldClose()) {  // true when user hits the window close button (Esc is repurposed below)

        // 1. TIME
        // dt = seconds since last frame. Multiply all movement by this
        // so speed is consistent regardless of framerate.
        float dt = GetFrameTime();

        // MARK: TITLE / GAME OVER SCREENS
        // Placeholder front/end screens. Each handles its own input + draw and
        // skips the rest of the loop; PLAYING (below) is the original game body.
        if (screen == GameScreen::TITLE) {
            if (startPressed()) startGame();
            BeginDrawing();
                ClearBackground(BLACK);
                DrawCentered("PLATFORMZ", 240, 80, RAYWHITE);
                DrawCentered("Press any key to start", 360, 20, GRAY);
            EndDrawing();
            continue;
        }
        if (screen == GameScreen::GAME_OVER) {
            if (startPressed()) returnToTitle();
            BeginDrawing();
                ClearBackground(BLACK);
                // Greyscale blit of the last rendered world frame (sceneTarget
                // persists), then the overlay - the dead-world look carries over.
                BeginShaderMode(grayscaleShader);
                    DrawTexturePro(sceneTarget.texture,
                        {0, 0, (float)sceneTarget.texture.width, -(float)sceneTarget.texture.height},
                        {0, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0.0f, WHITE);
                EndShaderMode();
                DrawCentered("GAME OVER", 240, 80, RED);
                DrawCentered("Press any key to return to title", 360, 20, GRAY);
            EndDrawing();
            continue;
        }

        // 2. UPDATE
        // Read input, mutate game state. No drawing happens here. In networked
        // mode the SERVER owns the sim - we only send input and apply the state
        // it broadcasts. In local mode we host the sim ourselves (unchanged).
        PlayerInput in{};                  // this frame's local intent (also read by the HUD)
        Player*     localPlayer = nullptr; // the player the camera + HUD follow (null until connected)
        int         localIndex  = networked ? myIndex : 0;

        if (networked) {
            // Gather input and predict look locally. Position is server-
            // authoritative, but a mouse-look that waited a full round-trip
            // would be unusable - so we accumulate yaw/pitch from the mouse here
            // (same math as Player::updateLook) and send the absolute values.
            in = PollLocalInput();
            if (!IsCursorHidden()) in = PlayerInput{}; // cursor freed (Esc): no look/move/fire
            predYaw   += in.lookDelta.x * 0.0025f;   // 0.0025 = Player::lookSensitivity
            predPitch -= in.lookDelta.y * 0.0025f;
            predPitch  = Clamp(predPitch, -89.0f * DEG2RAD, 89.0f * DEG2RAD);

            if (net.isOpen())
                net.send(serializeInput(inputSeq++, in, predYaw, predPitch));

            // Snapshot asteroid positions by id BEFORE applying state. The server
            // destroys and erases an asteroid within one tick, so it never sends
            // dead=true - an asteroid simply vanishes from the synced set. We spawn
            // its break-up burst client-side at the last position we knew for any id
            // that disappears this frame (sparks aren't networked - purely local VFX).
            std::unordered_map<uint32_t, Vector3> prevAsteroidPos;
            for (const Asteroid& a : gameSpace.getAsteroids())
                prevAsteroidPos[a.id] = a.position;

            // Apply the newest server state to our local GameSpace.
            for (const std::string& frame : net.poll()) {
                ServerMessage m = applyMessage(frame, gameSpace);
                if (m.type == ServerMessage::Type::Welcome) {
                    myIndex = m.playerId;
                    TraceLog(LOG_INFO, "Joined as player slot %d", myIndex);
                }
            }
            localIndex = myIndex;

            // Asteroid break-up bursts: any id we had last frame that's now gone
            // was destroyed on the server. Spawn the same burst the local sim does.
            {
                std::unordered_set<uint32_t> present;
                for (const Asteroid& a : gameSpace.getAsteroids()) present.insert(a.id);
                for (const auto& [id, pos] : prevAsteroidPos)
                    if (present.find(id) == present.end())
                        gameSpace.spawnEliminationBurst(pos);
            }

            // Player elimination bursts. Players are never erased (alive=false is
            // synced and persists), so a per-player flag stops the burst re-firing.
            for (Player& player : gameSpace.getPlayers()) {
                if (!player.isAlive && !player.deathBurstSpawned) {
                    gameSpace.spawnEliminationBurst(player.position);
                    player.deathBurstSpawned = true;
                }
            }

            // Tick the bursts: the server owns every other object, but sparks are
            // a local-only effect, so the client drifts/fades/retires them itself.
            gameSpace.updateSparks(dt);

            std::vector<Player>& players = gameSpace.getPlayers();
            if (localIndex >= 0 && localIndex < (int)players.size()) {
                // Seed prediction from the server spawn orientation once so the
                // first frame doesn't snap the view.
                if (!predInit) {
                    predYaw = players[localIndex].yaw;
                    predPitch = players[localIndex].pitch;
                    predInit = true;
                }
                // Override the local player's look with the prediction (the
                // server echo lags); all other fields stay server-authoritative.
                players[localIndex].yaw   = predYaw;
                players[localIndex].pitch = predPitch;
                localPlayer = &players[localIndex];

                // Predict our OWN rocket-launch sound locally so it's instant
                // instead of waiting a round-trip. Gated on synced ammo so we
                // don't play it when the server will reject the shot (no ammo).
                // The server's echo of this launch (owner == us) is filtered out
                // in the audio drain below, so it never doubles.
                if (in.fire && players[localIndex].ammo > 0)
                    audioQueue.push(fxTable[FX_ROCKET_LAUNCH], localPlayer->position, /*isLocal=*/true);

                // Hit flash: the server owns damage, so trigger the glitch when
                // our health drops between state packets (flashIntensity() is
                // always 0 on the client - it never ticks our flashTimer).
                int hp = players[localIndex].health;
                if (prevHealth >= 0 && hp < prevHealth) {
                    netHurt = 1.0f;
                    // Predict our own hit/death sound instantly (same filtering
                    // as the launch above keeps the server echo from doubling).
                    audioQueue.push(fxTable[hp > 0 ? FX_PLAYER_HIT : FX_PLAYER_DEATH],
                                    localPlayer->position, /*isLocal=*/true);
                }
                prevHealth = hp;
            }
            netHurt = fmaxf(0.0f, netHurt - dt / 0.5f); // decay over flash_duration (0.5s)
            // Distinguish players visually (you = default cyan, others = magenta),
            // matching the convention GameSpace::generate() uses in local mode.
            for (int i = 0; i < (int)players.size(); ++i) {
                if (i == localIndex) continue;
                players[i].color_outline = {255, 0, 200, 255};
                players[i].color_fill    = {255, 0, 200, 40};
            }
            // Tick reticles: snap ours, smooth everyone else's.
            for (int i = 0; i < (int)players.size(); ++i)
                players[i].updateReticle(dt, i != localIndex);
        } else {
            // --- LOCAL SINGLE-PLAYER: host the sim (original game loop) ---
            Player& player = gameSpace.getPlayers()[0];

            // Gather this frame's intent into a source-agnostic struct, then
            // apply it - the same struct the networked server applies remotely.
            in = PollLocalInput();
            if (!IsCursorHidden()) in = PlayerInput{}; // cursor freed (Esc): no look/move/fire
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

            gameSpace.updatePositions(dt);
            RunCollisionChecks(gameSpace, collisionGrid);   // detection + response
            gameSpace.updateActiveObjects();                // erase destroyed/finished

            // Reticles follow the player's FINAL (post-collision) position;
            // smoothed for non-local players, snapped for the local one.
            for (int i = 0; i < (int)players.size(); ++i)
                players[i].updateReticle(dt, i != 0); // index 0 is the local player

            localPlayer = &gameSpace.getPlayers()[0];
        }
        
        // MARK: AUDIO EVENT DRAIN
        // Turn this frame's networked/local audio events into queued sounds.
        // Networked: filled by applyMessage() from the packet. Local: filled by
        // the sim (input/collisions/updateActiveObjects). We skip events we
        // already predicted locally for ourselves (own launch / hit / death) so
        // the server's echo of them doesn't double up; everything else (incl.
        // our own explosions, which we can't predict) plays from the stream.
        {
            std::vector<Player>& players = gameSpace.getPlayers();
            uint32_t myId = (localIndex >= 0 && localIndex < (int)players.size())
                          ? players[localIndex].id : 0u;
            for (const NetAudioEvent& ev : gameSpace.getAudioEvents()) {
                if (ev.fx < 0 || ev.fx >= FX_COUNT) continue;
                // Only filter in networked mode: that's the only mode that
                // predicts our own events (the local host produces every event
                // via the sim and predicts nothing, so it must play them all).
                bool predicted = networked && ev.owner == myId && myId != 0u &&
                    (ev.fx == FX_ROCKET_LAUNCH || ev.fx == FX_PLAYER_HIT || ev.fx == FX_PLAYER_DEATH);
                if (predicted) continue;
                audioQueue.push(fxTable[ev.fx], ev.pos);
            }
            gameSpace.getAudioEvents().clear();
        }

        // MARK: AUDIO QUEUE FLUSH
        // Play all queued sound events after the state updates. Skip while
        // networked-but-not-yet-connected: localPlayer is still null until the
        // first server packet (the connecting-screen guard below handles that
        // frame), and nothing has queued a sound yet anyway.
        if (localPlayer != nullptr)
            audioQueue.flush(localPlayer ? *localPlayer : gameSpace.getPlayers()[0]);

        if (IsKeyPressed(KEY_ESCAPE)) {
            // toggle cursor capture so you can alt-tab / quit comfortably
            if (IsCursorHidden()) EnableCursor(); else DisableCursor();
        }

        // Networked mode before the server's welcome/first state arrives: there's
        // no local player yet, so show a connecting screen and skip the world draw.
        if (localPlayer == nullptr) {
            BeginDrawing();
                ClearBackground(BLACK);
                const char* msg = net.isOpen() ? "JOINING GAME..." : "CONNECTING TO SERVER...";
                DrawText(msg, 20, 20, 20, RAYWHITE);
                DrawText(serverUrl.c_str(), 20, 48, 14, DARKGRAY);
                if (!net.lastError().empty())
                    DrawText(net.lastError().c_str(), 20, 70, 14, RED);
            EndDrawing();
            continue;
        }
        // From here the local player exists; alias it so the draw code below is
        // identical for both modes (local mode set localPlayer = players[0]).
        Player& player = *localPlayer;
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
            BeginMode3D(CameraFromPlayer(player));
                gameSpace.draw(localIndex); // skip drawing our own body (first-person)
            EndMode3D();
        EndTextureMode();

        // Screen pass. Everything between BeginDrawing/EndDrawing hits the screen.
        BeginDrawing();
            ClearBackground(BLACK);

            // Networked: damage is server-side, so flashIntensity() is always 0
            // here - use the locally-tracked netHurt (set on a health drop above).
            float hurt = networked ? netHurt : player.flashIntensity(); // 1.0 just after a hit -> 0.0
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
            // draw onscreen text HUD. (Death now transitions to the GAME_OVER
            // screen below, so the dead frame still renders with its glitch but
            // doesn't draw the live HUD.)
            if (player.isAlive) {
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

        // The player just died: this frame already rendered with the death
        // glitch/greyscale; flip to GAME_OVER so the next frame shows the
        // end screen over the frozen world.
        if (!player.isAlive) screen = GameScreen::GAME_OVER;

        // Loop repeats. raylib handles vsync/frame pacing via SetTargetFPS.
    }

    // --- Teardown (runs once, after the loop exits) ---
    for (audioFX& fx : fxTable) fx.unload();
    CloseAudioDevice();
    UnloadRenderTexture(sceneTarget);
    UnloadShader(grayscaleShader);
    CloseWindow();
    return 0;
}
