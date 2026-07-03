#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include "gamespace.h"
#include "shapes.h"
#include "collisions.h"
#include "camera.h"
#include "input.h"
#include "ui.h"        // immediate-mode menu widgets (title screen)
#include "audio.h"     // sound FX que, load/unload, trigger
#include "net_client.h" // multiplayer: WebSocket client
#include "wire.h"       // multiplayer: input serialize / state apply
#include "bot_logic.h"   // bot AI decision tree

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
    EnableCursor(); // start on the TITLE screen with a free cursor; the mouse is
                    // captured on entry to PLAYING (startGame) and freed again on
                    // the menu screens (returnToTitle / game-over trigger)

    // MARK: AUDIO
    // raylib's mixer must be up before any sound loads. The fxTable maps each
    // AudioFXId (the shared client/server wire id) to a loaded sound; game
    // events arrive as ids (locally or over the network) and index into it.
    InitAudioDevice();
    SetMasterVolume(1.0f);
    audioFX fxTable[FX_COUNT] = {
        audioFX("assets/sounds/rocket_launch.wav",  1.0f), // FX_ROCKET_LAUNCH
        audioFX("assets/sounds/explosion.wav",      1.0f), // FX_EXPLOSION
        audioFX("assets/sounds/asteroid_break.wav", 1.0f, true, false), // FX_ASTEROID_BREAK (this is more of a bonus sound now)
        audioFX("assets/sounds/player_hit.wav",     1.0f, true, false), // FX_PLAYER_HIT (local player only, not spatial)
        audioFX("assets/sounds/player_death.wav",   1.0f, true, false), // FX_PLAYER_DEATH (local player only, not spatial)
        audioFX("assets/sounds/no_ammo.wav",        0.5f, true, false)  // FX_NO_AMMO (local player only, not spatial)
    };
    for (audioFX& fx : fxTable) fx.load();

    // The 3D scene is rendered into this off-screen target each frame so the
    // finished image can be sampled and distorted on the way to the screen -
    // that's what drives the damage glitch (a draw-on-top overlay can't
    // scramble the pixels that are already there).
    RenderTexture2D sceneTarget = LoadRenderTexture(screenWidth, screenHeight);

    // Greyscale post-process, used on death. Fragment-only shader (the 0 vertex
    // arg means "use raylib's default vertex shader"); kept inline so there's no
    // extra file to ship. colDiffuse is applied AFTER the luminance, so any draw
    // tint still shows through (e.g. the glitch's red/blue split copies keep a
    // faint fringe). Two dialects: desktop is OpenGL 3.3 (GLSL 330); the web build
    // (raylib PLATFORM_WEB = OpenGL ES2 / WebGL1) needs GLSL ES 100, else this
    // won't compile and BeginShaderMode would bind no valid program.
#if defined(__EMSCRIPTEN__)
    const char* grayscaleFrag =
        "#version 100\n"
        "precision mediump float;\n"
        "varying vec2 fragTexCoord;\n"
        "varying vec4 fragColor;\n"
        "uniform sampler2D texture0;\n"
        "uniform vec4 colDiffuse;\n"
        "void main() {\n"
        "    vec4 texel = texture2D(texture0, fragTexCoord);\n"
        "    float lum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));\n" // Rec.601 luma
        "    gl_FragColor = vec4(vec3(lum), texel.a) * colDiffuse * fragColor;\n"
        "}\n";
#else
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
#endif
    Shader grayscaleShader = LoadShaderFromMemory(0, grayscaleFrag);
    // Composite through the shader only if it actually compiled+linked. On failure
    // raylib leaves an unusable program; BeginShaderMode would then break every
    // draw inside it (the glitch FX). When invalid we skip it and still show the
    // glitch - degrading to "no greyscale" instead of "nothing". Guards every
    // BeginShaderMode(grayscaleShader) below.
    const bool grayscaleOK = IsShaderValid(grayscaleShader) &&
                             grayscaleShader.id != rlGetShaderIdDefault();

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
    bool      consumeFirstLook = false; // drop the first mouse-look delta after a cursor
                                        // lock (GetMouseDelta jumps the frame after
                                        // DisableCursor), so the centered spawn
                                        // orientation isn't clobbered.
    bool      consumeFirstFire = false; // drop the first frame's fire after entering a
                                        // match - the click that starts/restarts the game
                                        // can land on the first play frame and would
                                        // otherwise be read as a rocket (issue #4).
    int       prevHealth = -1;    // last frame's health, to detect a server-side hit
    float     netHurt   = 0.0f;   // client hit-flash intensity (the server owns damage,
                                  // so flashIntensity() is always 0 here - drive it locally)

    // MARK: BOT STATE & TREE
    // One wander-bot per non-local player (players[1..]) - LOCAL MODE ONLY.
    // Index 0 is the local human. In networked mode there are no bots; empty
    // slots simply stay unoccupied until another client connects.
    std::vector<BotState> botStates;
    // Per-bot decision state for the shared behaviour tree's LatchedSelector
    // (branch latch + timer). Sized alongside botStates in startGame().
    std::vector<std::vector<BotDecision>> botDecisions; // per bot -> one BotDecision per LatchedSelector (LATCH_COUNT slots)
    // Per-bot personality (aggression/accuracy), assigned once at spawn in
    // startGame() and read by the tree nodes. Also sized alongside botStates.
    std::vector<BotProfile> botProfiles;

    /// BOT DECISION TREE
    // One latch slot per LatchedSelector in the tree. Each bot gets a
    // BotDecision vector sized to LATCH_COUNT so nested/sibling latches keep
    // independent state (see LatchedSelector). Add a new LatchedSelector? Give
    // it an id here.
    enum LatchId { LATCH_MOVEMENT, LATCH_MOVE_TO_SAFETY,
                   LATCH_ATTACK_STYLE, LATCH_FIRE,
                   LATCH_KITE_CHANCE, LATCH_RETREAT_CD, LATCH_COUNT };
    /// Instantiate leaf nodes
    IsLowFuel<Player>      isLowFuel;
    IsLowHealth<Player>    isLowHealth;
    FindLineOfSight<Player> findLineOfSight;
    FindCover<Player>      findCover;
    MoveToPlayer<Player>   moveToPlayer;
    MoveFromPlayer<Player>   moveFromPlayer;
    MoveToPlatform<Player> moveToPlatform;
    FireAtPlayer<Player>   fireAtPlayer;
    AttackAsteroid<Player> attackAsteroid;
    AvoidAsteroid<Player>  avoidAsteroid;
    Idle<Player>           idle;

    // Compose the tree. `movement` is a LatchedSelector: it re-evaluates WHICH
    // branch wins only every BOT_TICK_TIME, but the chosen action still ticks
    // every frame. fireAtTarget sits in the top Parallel, so aim/fire stay
    // per-frame regardless of the movement decision cadence.
    LatchedSelector<Player>        fireAtTarget(LATCH_FIRE, { &fireAtPlayer, &attackAsteroid });
    // Retreat: a hurt bot doesn't ALWAYS flee (Chance gates the kite), and once
    // it does retreat it can't again for a few seconds (Cooldown wraps the whole
    // retreat) — no fight/flight yo-yo. avoidAsteroid stays a hard priority.
    Chance<Player>                 maybeKite(LATCH_KITE_CHANCE, 0.6f, &moveFromPlayer);
    LatchedSelector<Player>        moveToSafety(LATCH_MOVE_TO_SAFETY, { &avoidAsteroid, &maybeKite, &findCover });
    Cooldown<Player>               retreatGate(LATCH_RETREAT_CD, 4.0f, &moveToSafety);
    Sequence<Player>               lowHealthResponse({ &isLowHealth, &retreatGate });
    Sequence<Player>               lowFuelResponse({ &isLowFuel, &moveToPlatform, &idle });
    // Attack: personality-weighted random pick between sniping from range
    // (findLineOfSight) and closing in (moveToPlayer) — aggressive bots close,
    // timid bots hold. avoidAsteroid is a hard priority ahead of either.
    WeightedRandomSelector<Player> attackStyle(LATCH_ATTACK_STYLE, {
        { &findLineOfSight, [](Blackboard<Player>& bb){ return 1.0f - bb.profile.aggression; } },
        { &moveToPlayer,    [](Blackboard<Player>& bb){ return bb.profile.aggression; } },
    });
    Selector<Player>               attack({ &avoidAsteroid, &attackStyle });
    LatchedSelector<Player>        movement(LATCH_MOVEMENT, { &lowHealthResponse, &lowFuelResponse, &attack });
    Parallel<Player>               botTree({ &movement, &fireAtTarget });


    // MARK: SCREEN STATE
    // The loop runs in one of three screens. TITLE and GAME_OVER are simple
    // placeholder overlays handled at the top of the loop; PLAYING is the
    // original game body (update -> draw). Play is gated by TITLE and ends back
    // there via GAME_OVER, so the world is set up on each PLAYING entry (not
    // before the loop) and torn down on the way back to the title.
    enum class GameScreen { TITLE, PLAYING, GAME_OVER };
    GameScreen screen = GameScreen::TITLE;
    float gameOverTimer = GAME_OVER_TIMER; // seconds since the last player died, to delay the GAME_OVER screen so the player sees the death FX
    bool  netMatchOver  = false; // networked: latched once the server reports the match ended, so the gameOverTimer countdown survives packet-less frames

    // Title-screen menu state (widgets live in ui.h). The name is local-only for
    // now (input + visuals); syncing it over the network is a later step.
    std::string playerName  = "PLAYER";
    bool        nameFocused = true;  // text field owns keyboard focus on entry (type without a click)
    bool        namePristine = true; // still the untouched "PLAYER" default; first keystroke clears it
    bool        showControls = false; // controls popup is open
    bool        showOptions  = false; // options popup is open (placeholder)

    // Networked client connects once, on launch: the title screen then acts as a
    // live lobby (the server owns the world and only starts it on request). Local
    // mode has no server.
    if (networked) {
        net.connect(serverUrl); // non-blocking; IXWebSocket retries on its own thread
        TraceLog(LOG_INFO, "Networked mode: connecting to %s", serverUrl.c_str());
    }

    // START from the title. Local: stand up a fresh run with the chosen map preset
    // (small/medium/large). Networked: ask the server to start/restart a match -
    // the screen flips to PLAYING when the server's phase says so (see the loop).
    auto startGame = [&](float halfSize, int platforms, int asteroids) {
        if (networked) {
            // Send the chosen map preset; the server applies it before generating
            // the world (first press wins for the round).
            if (net.isOpen()) net.send(serializeStart(halfSize, platforms, asteroids));
            return;
        }
        gameSpace.configureMap(halfSize, platforms, asteroids); // apply the chosen map preset
        gameSpace.generate(); // platforms, asteroids, and player slots
        botStates.assign(gameSpace.getPlayers().size() - 1, BotState{});
        botDecisions.assign(gameSpace.getPlayers().size() - 1, std::vector<BotDecision>(LATCH_COUNT));
        botProfiles.assign(gameSpace.getPlayers().size() - 1, BotProfile{});
        // Local mode owns its sim: mark/color the wander-bot slots (index 1+).
        // (Networked mode takes isBot from the server over the wire instead.)
        std::vector<Player>& ps = gameSpace.getPlayers();
        // Slot 0 is the local human; carry the title-screen name onto it so the
        // scoreboard shows it (networked play gets this from the server instead).
        ps[0].name = playerName.empty() ? "PLAYER" : playerName;
        // Personality spread widens with bot count: a lone bot ~= BOT_DIFFICULTY,
        // a crowd fans out around it. Seeded from player.id so the same map
        // replays the same personalities.
        int nBots = (int)ps.size() - 1;
        float spread = nBots > 0 ? BOT_PERSONALITY_SPREAD * (nBots - 1) / nBots : 0.0f;
        for (size_t i = 1; i < ps.size(); ++i) {
            ps[i].isBot = true;
            ps[i].color_outline = BOT_OUTLINE_COLOR;
            ps[i].color_fill    = BOT_FILL_COLOR;
            // Same NATO label the title-screen lobby shows for this bot slot.
            ps[i].name = BOT_NAME_STRINGS[(i - 1) % BOT_NAME_COUNT];
            // Seed each bot's personality once, deterministically from its id.
            std::mt19937 rng(ps[i].id);
            std::uniform_real_distribution<float> jitter(-spread, spread);
            botProfiles[i - 1].aggression = Clamp(BOT_DIFFICULTY + jitter(rng), 0.0f, 1.0f);
            botProfiles[i - 1].accuracy   = Clamp(BOT_DIFFICULTY + jitter(rng), 0.0f, 1.0f);
        }
        gameOverTimer = GAME_OVER_TIMER; // fresh game-over countdown for this run
        DisableCursor(); // capture the mouse for free-look while playing
        consumeFirstLook = true; // swallow the cursor-lock delta on the first play frame
        consumeFirstFire = true; // swallow the start click so it isn't read as a rocket
        screen = GameScreen::PLAYING;
    };

    // Networked: the server's phase just went PLAYING (we started, a peer started,
    // or we joined a running match). Reset per-match prediction/flash state (NOT
    // inputSeq - it stays monotonic across matches so the server doesn't discard
    // our packets) and enter the match. myIndex persists; predInit=false re-seeds
    // the look from the server's spawn orientation.
    auto enterNetworkedMatch = [&]() {
        predInit = false; prevHealth = -1; netHurt = 0.0f;
        netMatchOver = false; gameOverTimer = GAME_OVER_TIMER; // fresh game-over countdown for this match
        DisableCursor();
        consumeFirstLook = true; // swallow the cursor-lock delta on the first play frame
        consumeFirstFire = true; // swallow the start click so it isn't read as a rocket
        screen = GameScreen::PLAYING;
    };

    // Networked: drain server frames (apply state, track our slot), returning the
    // latest match phase. Used by the lobby/game-over screens, which otherwise
    // wouldn't poll the socket, so they can react to phase changes.
    auto pumpNet = [&]() -> ServerMessage::Phase {
        ServerMessage::Phase phase = ServerMessage::Phase::Unknown;
        for (const std::string& frame : net.poll()) {
            ServerMessage m = applyMessage(frame, gameSpace);
            if (m.type == ServerMessage::Type::Welcome) {
                myIndex = m.playerId;
                net.send(serializeName(playerName)); // attach our display name to the slot
            }
            else if (m.type == ServerMessage::Type::State) phase = m.phase;
        }
        return phase;
    };

    // GAME_OVER/title -> TITLE. Local: wipe the world for a clean restart.
    // Networked: stay connected (back to the lobby) so START can restart; the
    // server owns the world and resyncs it. The NetClient dtor closes on exit.
    auto returnToTitle = [&]() {
        if (!networked) gameSpace.clear();
        EnableCursor(); // free the cursor for the title menu
        nameFocused = true; // re-focus the name field so the player can type without a click
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

        // MARK: TITLE SCREEN
        // Placeholder front/end screens. Each handles its own input + draw and
        // skips the rest of the loop; PLAYING (below) is the original game body.
        if (screen == GameScreen::TITLE) {
            // Networked: the title screen is the lobby - pump the server so the
            // player list stays live and we follow the server into a match (we
            // started it, a peer did, or we joined one already running).
            if (networked && pumpNet() == ServerMessage::Phase::Playing) {
                enterNetworkedMatch();
                continue;
            }
            // Esc closes an open popup (no cursor toggle on the menu).
            if (showControls && IsKeyPressed(KEY_ESCAPE)) showControls = false;
            if (showOptions  && IsKeyPressed(KEY_ESCAPE)) showOptions  = false;
            // Snapshot each popup state at frame start: CLOSE is only handled if the
            // popup was ALREADY open, so the click that opens it can't also close
            // it on the same frame (the open button and CLOSE overlap on screen).
            const bool controlsWasOpen = showControls;
            const bool optionsWasOpen  = showOptions;
            const bool uiEnabled = !showControls && !showOptions; // either popup is modal

            BeginDrawing();
                ClearBackground(BLACK);
                UiTextCentered("PLATFORMZ", screenWidth, 110, 80, RAYWHITE);

                // Name entry (local-only for now).
                UiTextCentered("NAME", screenWidth, 215, 20, ui::OUTLINE);
                Rectangle nameBox = {350, 240, 300, 40};
                // Push every edit to the server so the latest typed name wins
                // (the welcome already sent a baseline before this field changed).
                if (UiTextField(nameBox, playerName, nameFocused, 16, 20, &namePristine) &&
                    networked && net.isOpen())
                    net.send(serializeName(playerName));

                // Players panel. Local: GAMESPACE_NUMBER_OF_PLAYERS slots - slot 1
                // is the human, the rest bot-filled (local play spawns 1 human +
                // N-1 bots). Networked: the live lobby - connected slots from the
                // server (isConnected), yours marked (YOU). Panel height tracks the
                // row count.
                std::vector<Player>& titlePlayers = gameSpace.getPlayers();
                int rowsShown;
                if (networked) {
                    rowsShown = 0;
                    for (const Player& p : titlePlayers) if (p.isConnected) rowsShown++;
                    if (rowsShown == 0) rowsShown = 1; // "waiting" line
                } else {
                    rowsShown = GAMESPACE_NUMBER_OF_PLAYERS;
                }
                const float rowH = 24.0f, headerH = 30.0f;
                Rectangle playersBox = {350, 300, 300, headerH + rowsShown * rowH + 10.0f};
                UiPanel(playersBox);
                DrawText("PLAYERS", (int)playersBox.x + 10, (int)playersBox.y + 8, 14, ui::OUTLINE);
                if (networked) {
                    int row = 0;
                    for (int i = 0; i < (int)titlePlayers.size(); ++i) {
                        if (!titlePlayers[i].isConnected) continue;
                        int ry = (int)(playersBox.y + headerH + row * rowH);
                        bool you = (i == myIndex);
                        // Local row shows the live-typed name; other rows show the
                        // server-synced name (updates live as they type), falling
                        // back to a slot label until they've set one.
                        const char* shown = you
                            ? (playerName.empty() ? "PLAYER" : playerName.c_str())
                            : (titlePlayers[i].name.empty() ? TextFormat("PLAYER %d", i + 1)
                                                            : titlePlayers[i].name.c_str());
                        DrawText(TextFormat("%d. %s%s", i + 1, shown, you ? " (YOU)" : ""),
                                 (int)playersBox.x + 10, ry, 18, you ? RAYWHITE : ui::OUTLINE);
                        row++;
                    }
                    if (row == 0)
                        DrawText("Waiting for players...", (int)playersBox.x + 10,
                                 (int)(playersBox.y + headerH), 18, GRAY);
                } else {
                    for (int i = 0; i < rowsShown; ++i) {
                        int ry = (int)(playersBox.y + headerH + i * rowH);
                        if (i == 0)
                            DrawText(TextFormat("1. %s (YOU)", playerName.empty() ? "PLAYER" : playerName.c_str()),
                                     (int)playersBox.x + 10, ry, 18, RAYWHITE);
                        else
                            DrawText(TextFormat("%d. %s", i + 1, BOT_NAME_STRINGS[(i - 1) % BOT_NAME_COUNT]),
                                     (int)playersBox.x + 10, ry, 18, ui::OUTLINE);
                    }
                }

                // Start buttons (map-size presets) below the variable-height panel.
                // Same three sizes in both modes: local generates the world; in
                // networked play the chosen preset rides the start request and the
                // server builds it (first press wins). Networked gates on being
                // connected with a slot; local is always ready.
                float startY = playersBox.y + playersBox.height + 20.0f;
                bool ready = !networked || (net.isOpen() && myIndex >= 0);
                if (ready) {
                    Rectangle bs = {210, startY, 180, 50};
                    Rectangle bm = {410, startY, 180, 50};
                    Rectangle bl = {610, startY, 180, 50};
                    if (uiEnabled && UiButton(bs, "SMALL"))  startGame(mapSizePresets["SMALL"].halfSize, mapSizePresets["SMALL"].numPlatforms, mapSizePresets["SMALL"].numAsteroids);
                    if (uiEnabled && UiButton(bm, "MEDIUM")) startGame(mapSizePresets["MEDIUM"].halfSize, mapSizePresets["MEDIUM"].numPlatforms, mapSizePresets["MEDIUM"].numAsteroids);
                    if (uiEnabled && UiButton(bl, "LARGE")) startGame(mapSizePresets["LARGE"].halfSize, mapSizePresets["LARGE"].numPlatforms, mapSizePresets["LARGE"].numAsteroids);
                } else {
                    UiTextCentered(net.isOpen() ? "JOINING..." : "CONNECTING...",
                                   screenWidth, (int)startY + 14, 20, GRAY);
                }

                // Opening a modal must leave the cursor free (the popups are
                // click-driven, especially OPTIONS). The title screen is already a
                // free-cursor state, but guard explicitly so this holds if a modal
                // is ever opened from a captured (in-game) context.
                Rectangle controlsBtn = {400, startY + 64.0f, 200, 44};
                if (uiEnabled && UiButton(controlsBtn, "CONTROLS")) { showControls = true; if (IsCursorHidden()) EnableCursor(); }
                Rectangle optionsBtn = {400, startY + 116.0f, 200, 44};
                if (uiEnabled && UiButton(optionsBtn, "OPTIONS")) { showOptions = true; if (IsCursorHidden()) EnableCursor(); }

                // Controls popup, drawn last so it sits on top. Opaque panel
                // (UiModalPanel) so the dimmed title UI doesn't bleed through.
                if (showControls) {
                    DrawRectangle(0, 0, screenWidth, screenHeight, Fade(BLACK, 0.7f));
                    Rectangle m = {250, 170, 500, 360};
                    UiModalPanel(m);
                    UiTextCentered("CONTROLS", screenWidth, (int)m.y + 20, 30, ui::OUTLINE);
                    const char* lines[] = {
                        "WASD          move",
                        "Mouse         look",
                        "Left click    fire rocket",
                        "Space         jetpack (up)",
                        "Left Shift    stronger (earth) gravity",
                        "Esc           toggle cursor capture",
                    };
                    int ly = (int)m.y + 80;
                    for (const char* ln : lines) { DrawText(ln, (int)m.x + 40, ly, 18, RAYWHITE); ly += 34; }
                    Rectangle closeBtn = {(float)(screenWidth / 2 - 70), m.y + m.height - 60, 140, 40};
                    if (controlsWasOpen && UiButton(closeBtn, "CLOSE")) showControls = false;
                }

                // Options popup (placeholder for now), same opaque modal style.
                if (showOptions) {
                    DrawRectangle(0, 0, screenWidth, screenHeight, Fade(BLACK, 0.7f));
                    Rectangle m = {250, 170, 500, 360};
                    UiModalPanel(m);
                    UiTextCentered("OPTIONS", screenWidth, (int)m.y + 20, 30, ui::OUTLINE);
                    const char* lines[] = {
                        "Master volume       coming soon",
                        "Mouse sensitivity   coming soon",
                        "Invert Y            coming soon",
                        "Field of view       coming soon",
                    };
                    int ly = (int)m.y + 90;
                    for (const char* ln : lines) { DrawText(ln, (int)m.x + 40, ly, 18, GRAY); ly += 40; }
                    Rectangle closeBtn = {(float)(screenWidth / 2 - 70), m.y + m.height - 60, 140, 40};
                    if (optionsWasOpen && UiButton(closeBtn, "CLOSE")) showOptions = false;
                }
            EndDrawing();
            continue;
        }
        // MARK: GAME_OVER SCREEN
        if (screen == GameScreen::GAME_OVER) {
            // Networked: keep pumping the server. If a match (re)starts (a peer
            // pressed START, or we did after returning to the lobby), follow it in.
            if (networked && pumpNet() == ServerMessage::Phase::Playing) {
                enterNetworkedMatch();
                continue;
            }
            if (startPressed()) returnToTitle();
            BeginDrawing();
            ClearBackground(BLACK);
            
            Color scoreColor = BLUE;
            std::vector<Player>& players = gameSpace.getPlayers();
            int localIndex = networked ? myIndex : 0;
            // Outcome banner for the local player.
            if (localIndex >= 0 && localIndex < (int)players.size()) {
                if (players[localIndex].isAlive) {
                    DrawCentered("GAME OVER", 240, 80, BLUE);
                    DrawCentered("You survived!", 360, 20, BLUE);
                    scoreColor = GRAY;
                }
                else {
                    // Greyscale blit of the last rendered world frame (sceneTarget
                    // persists), then the overlay - the dead-world look carries over.
                    if (grayscaleOK) BeginShaderMode(grayscaleShader);
                        DrawTexturePro(sceneTarget.texture,
                            {0, 0, (float)sceneTarget.texture.width, -(float)sceneTarget.texture.height},
                            {0, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0.0f, WHITE);
                    if (grayscaleOK) EndShaderMode();
                    DrawCentered("GAME OVER", 240, 80, RED);
                    DrawCentered("You were eliminated.", 360, 20, RED);
                }
            }

            // Scoreboard - every connected player by name (drawn once). Empty
            // slots in the fixed player vector are skipped via isConnected.
            DrawCentered("SCORES:", 400, 20, scoreColor);
            int scoreRow = 0;
            for (int i = 0; i < (int)players.size(); ++i) {
                if (networked && !players[i].isConnected) continue;
                DrawCentered(TextFormat("%s: %d", players[i].name.c_str(), players[i].score),
                                440 + scoreRow * 20, 20, scoreColor);
                ++scoreRow;
            }
            DrawCentered("Press any key to return to title.", 440 + scoreRow * 20 + 10, 20, BLUE);
            
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
        ServerMessage::Phase netPhase = ServerMessage::Phase::Unknown; // latest server phase (networked); drives the game-over edge below

        if (networked) {
            // Gather input and predict look locally. Position is server-
            // authoritative, but a mouse-look that waited a full round-trip
            // would be unusable - so we accumulate yaw/pitch from the mouse here
            // (same math as Player::updateLook) and send the absolute values.
            in = PollLocalInput();
            if (!IsCursorHidden()) in = PlayerInput{}; // cursor freed (Esc): no look/move/fire
            if (consumeFirstLook) { in.lookDelta = {0, 0}; consumeFirstLook = false; } // drop cursor-lock jump
            if (consumeFirstFire) { in.fire = false; consumeFirstFire = false; } // drop the match-start click (issue #4)
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
                } else if (m.type == ServerMessage::Type::State) {
                    netPhase = m.phase; // track phase so we can detect the match ending
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
                    gameSpace.spawnEliminationBurst(player.position, player.color_outline);
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
            // Color each player slot by its index, matching GameSpace::spawnPlayers():
            // bots are magenta (BOT_*_COLOR), humans round-robin through HUMAN_PLAYER_COLORS.
            // Color isn't sent over the wire (it's static per slot); the `bot` flag is.
            for (int i = 0; i < (int)players.size(); ++i) {
                if (players[i].isBot) {
                    players[i].color_outline = BOT_OUTLINE_COLOR;
                    players[i].color_fill    = BOT_FILL_COLOR;
                } else {
                    Color c = HUMAN_PLAYER_COLORS[i % HUMAN_PLAYER_COLORS.size()];
                    c.a = 255;
                    players[i].color_outline = c;
                    c.a = 40;
                    players[i].color_fill = c;
                }
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
            if (consumeFirstLook) { in.lookDelta = {0, 0}; consumeFirstLook = false; } // drop cursor-lock jump
            if (consumeFirstFire) { in.fire = false; consumeFirstFire = false; } // drop the match-start click (issue #4)
            float gravity = in.earthGravity ? EARTH_GRAVITY : MOON_GRAVITY; // constants stay here
            ApplyPlayerInput(player, in, dt, gravity, gameSpace);

            // Drive the test bots (players[1..]) through the same input path with
            // randomly-generated wander input. Bots always use moon gravity - they
            // never hold the earth-gravity key.
            
            std::vector<Player>& players = gameSpace.getPlayers();
            for (int i = 1; i < (int)players.size(); ++i) {
                // Per bot, per frame: run the behaviour tree. botDecisions[i-1]
                // is this bot's LatchedSelector state. Bots may hold the
                // earth-gravity key (to descend), so derive gravity from the input.
                PlayerInput botIn = botInput(players[i], players[0], players,
                                             gameSpace.getPlatforms(), gameSpace.getAsteroids(),
                                             botTree, dt, botDecisions[i - 1], botProfiles[i - 1]);
                float gravity = botIn.earthGravity ? EARTH_GRAVITY : MOON_GRAVITY;
                ApplyPlayerInput(players[i], botIn, dt, gravity, gameSpace);
                // OG bot code (pre-BT) - replaced by the above botInput() call:
                // PlayerInput botIn = MakeBotInput(players[i], botStates[i - 1], dt);
                // ApplyPlayerInput(players[i], botIn, dt, MOON_GRAVITY, gameSpace);
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

        gameSpace.updateAsteroidSpin(dt); // advance tumble in both local and networked modes

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
                // Mark the event as local when we own it, so AudioQueue::push
                // doesn't drop our own localPlayerOnly sounds (e.g. FX_NO_AMMO,
                // and local-mode hit/death). Events owned by other players keep
                // isLocal=false and their localPlayerOnly sounds stay filtered.
                bool isLocal = ev.owner == myId && myId != 0u;
                audioQueue.push(fxTable[ev.fx], ev.pos, isLocal);
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

            if (!player.isAlive && grayscaleOK) BeginShaderMode(grayscaleShader);
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
            if (!player.isAlive && grayscaleOK) EndShaderMode();
// MARK: HUD
            // text size, x, y, color.
            // draw onscreen text HUD. (Death transitions to the GAME_OVER screen
            // below, which owns all death text, so the dead frame renders only its
            // glitched/greyscale frozen world here - no live HUD, no death text.)
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
        // MARK: game over trigger

        
        if (networked) {
            // Networked: the SERVER decides when the match ends (last-player-
            // standing). Mirror the local death-FX delay - count down before the
            // menu so the frozen end state (greyscale/glitch) is visible first.
            if (netPhase == ServerMessage::Phase::Playing) {
                // match (re)started or still live - cancel any pending countdown
                netMatchOver = false; gameOverTimer = GAME_OVER_TIMER;
            } else if (netPhase == ServerMessage::Phase::GameOver) {
                netMatchOver = true; // latch: survives packet-less (Unknown) frames
            }
            if (netMatchOver) {
                gameOverTimer -= dt;
                if (gameOverTimer <= 0.0f) {
                    EnableCursor(); // free the cursor for the game-over menu
                    screen = GameScreen::GAME_OVER;
                }
            }
        } else {
            int remaining_players = 0;
            int remaining_humans = 0;
            int remaining_bots = 0;
            int remaining_asteroids = (int)gameSpace.getAsteroids().size();
            
            if (gameSpace.getPlayers().size() == 1) {
                // specal case.  Solo player, no bots.  Game only end when player dies or all asteroids are gone.
                if (remaining_asteroids <= 0 || !gameSpace.getPlayers()[0].isAlive) {
                    gameOverTimer -= dt;
                    if (gameOverTimer <= 0.0f) {
                        EnableCursor(); // free the cursor for the game-over menu
                        screen = GameScreen::GAME_OVER;
                    }
                }
            } else {
                // main case.  Multiple players (bots and humans).  Game ends when only one player is left or all humans are dead.
                // determine number of remaining ALIVE players (bots included). Only
                // alive players count toward the human/bot tally - otherwise a dead
                // human still counts as a human and remaining_humans never hits 0.
                for (const Player& p : gameSpace.getPlayers()) {
                    if (!p.isAlive) continue;
                    remaining_players++;
                    if (p.isBot) remaining_bots++; else remaining_humans++;
                }
                // If there's only one player left or only bots left, start the game-over countdown.
                if (remaining_players <= 1 || remaining_humans <= 0 || (remaining_players <=1 && remaining_asteroids <= 0)) {
                    gameOverTimer -= dt;
                    if (gameOverTimer <= 0.0f) {
                        EnableCursor(); // free the cursor for the game-over menu
                        screen = GameScreen::GAME_OVER;
                    }
                }
            }
        }
        
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
