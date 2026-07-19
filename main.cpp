#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include "gamespace.h"
#include "shapes.h"
#include "collisions.h"
#include "camera.h"
#include "starfield.h" // distant starry-vista background (client-only)
#include <algorithm>   // std::nth_element (F3 perf overlay p95)
#include "input.h"
#include "ui.h"        // immediate-mode menu widgets (title screen)
#include "audio.h"     // sound FX que, load/unload, trigger
#include "jukebox.h"
#include "net_client.h" // multiplayer: WebSocket client
#include "wire.h"       // multiplayer: input serialize / state apply
#include "bot_logic.h"   // bot AI decision tree
#include "bot_controller.h" // shared bot orchestration (tree + per-slot state + drive)
#include "messages.h"    // transient on-screen message queue (kill-feed HUD)

#include <string>
#include <unordered_map>
#include <unordered_set>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h> // emscripten_run_script_string (read page URL)
// Tell shell.html whether a title-screen modal (CONTROLS/OPTIONS) is open, so its
// mousedown pointer-lock handler can skip grabbing the cursor for menu popups.
EM_JS(void, PlatformzSetModalOpen, (int open), { if (window.Module) Module.modalOpen = !!open; });
#else
inline void PlatformzSetModalOpen(int) {} // no-op on native builds
#endif

// Kill-feed HUD: draw the live messages centered at the bottom of the window,
// newest lowest, older ones stacked upward; each fades out over its final fadeTime
// seconds. 2D HUD call - run it in the screen pass (after EndMode3D), alongside the
// rest of the HUD. Draws nothing when the queue is empty. Lives here (the rendering
// TU) rather than on MessageQueue so messages.h stays free of raylib draw primitives
// and compiles into the headless server build.
static void DrawMessageQueue(MessageQueue& mq, int screenW, int screenH) {
    const int   fontSize  = 20;
    const int   lineStep  = fontSize + 8;
    const int   bottomPad = 40;    // gap above the window's bottom edge
    const float fadeTime  = 2.5f;  // seconds of fade at the tail of timeRemaining
    const int   outline   = 1;     // dark outline thickness in px

    std::vector<Message>& msgs = mq.getMessages();
    int y = screenH - bottomPad;   // newest sits lowest; older stack upward
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        const Message& msg = *it;
        float a = (msg.timeRemaining < fadeTime) ? msg.timeRemaining / fadeTime : 1.0f;
        if (a < 0.0f) a = 0.0f;
        const char* t = msg.text.c_str();
        int x = (screenW - MeasureText(t, fontSize)) / 2;
        // Dark outline behind the text so it stays legible over the 3D scene;
        // fades with the fill via the same alpha. 8 offsets = full surround,
        // not a one-sided drop shadow (reads better over arbitrary backgrounds).
        Color shadow = Fade(BLACK, a);
        for (int dx = -outline; dx <= outline; dx += outline)
            for (int dy = -outline; dy <= outline; dy += outline)
                if (dx || dy) DrawText(t, x + dx, y + dy, fontSize, shadow);
        DrawText(t, x, y, fontSize, Fade(msg.color, a));
        y -= lineStep;
    }
}


// PLATFORMZ runs in one of two modes:
//   ./platformz                      -> local single-player (host the sim here)
//   ./platformz ws://host:9000       -> networked client of an authoritative
//   ./platformz wss://host/...          server over WebSocket (TCP)
//   ./platformz udp://host:9000      -> same, but over UDP (native only; lower
//                                        latency, tolerant of packet loss)
// The URL scheme picks the transport (see net_client.h); the server speaks both
// at once, so udp:// and ws:// clients share a match. The networked path reuses
// the exact same headers; it just doesn't run the sim locally - the server owns
// it (see server/server_main.cpp).
//
// In the browser there's no command line, so the web build is always a
// networked client and the server URL comes from the page query string.
int main(int argc, char** argv) {
    bool        networked;
    std::string serverUrl;
    // Auto-fallback (native, baked-in server only): start on UDP and pivot to the
    // ws:// at the same host if the UDP handshake never completes. Left off for the
    // browser and for an explicit URL arg (which honors whatever scheme was given).
    bool        autoFallback  = false;
    std::string fallbackWsUrl;
    // "bench" CLI (native only): measurement-only launch that skips the title and
    // starts a local match of arbitrary size with bots + the F3 perf overlay on.
    bool  benchMode    = false;
    float benchHalf    = mapSizePresets["LARGE"].halfSize;
    int   benchPlat    = mapSizePresets["LARGE"].numPlatforms;
    int   benchRoid    = mapSizePresets["LARGE"].numAsteroids;
    int   benchPlayers = 4;
#if defined(__EMSCRIPTEN__)
    (void)argc; (void)argv;
    networked = true;
    // e.g. platformz.html?server=ws://192.168.1.20:9000
    // Default follows the page's own origin, scheme-aware: an https page must
    // use wss:// (mixed-content rules silently block ws:// there, leaving the
    // client stuck on CONNECTING), and TLS terminates at the reverse proxy on
    // the standard port, which forwards /ws to the game server (see
    // docs/deploy-vultr.md "HTTPS upgrade"). Plain http (LAN/dev) keeps
    // talking straight to the game server on :9000.
    // A ?key= param on the page (the invite link) is forwarded onto the socket
    // URL, where the server's join gate reads it during the HTTP upgrade. The
    // key is never baked into the wasm - the bundle is public; the invite link
    // is the private channel.
    serverUrl = [] {
        const char* s = emscripten_run_script_string(
            "(function(){"
            "var p = new URLSearchParams(location.search);"
            "var s = p.get('server') || "
            "(location.protocol === 'https:'"
            " ? 'wss://' + location.host + '/ws'"
            " : 'ws://' + location.hostname + ':9000');"
            "var k = p.get('key');"
            "if (k && s.indexOf('key=') < 0)"
            " s += (s.indexOf('?') < 0 ? '?' : '&') + 'key=' + encodeURIComponent(k);"
            "return s;})()");
        return std::string(s ? s : "");
    }();
#else
    // Native launch: a URL arg is an explicit override; the word "local" forces
    // single-player; no arg uses the baked-in server (if any) preferring UDP with a
    // WS pivot, else falls back to local single-player (dev default).
    std::string defaultHost = PLATFORMZ_DEFAULT_SERVER_HOST;
    if (argc > 1 && std::string(argv[1]) == "local") {
        networked = false;                             // explicit single-player
    } else if (argc > 1 && std::string(argv[1]) == "bench") {
        // Usage: ./platformz bench <halfSize> <platforms> <asteroids> [players]
        networked = false; benchMode = true;
        if (argc > 2) benchHalf    = std::stof(argv[2]);
        if (argc > 3) benchPlat    = std::stoi(argv[3]);
        if (argc > 4) benchRoid    = std::stoi(argv[4]);
        if (argc > 5) benchPlayers = std::stoi(argv[5]);
    } else if (argc > 1) {
        networked = true; serverUrl = argv[1];         // explicit URL: honor scheme, no auto-fallback
    } else if (!defaultHost.empty()) {                 // baked server: prefer UDP, allow a WS pivot
        networked = true;
        std::string port = PLATFORMZ_DEFAULT_SERVER_PORT;
        serverUrl     = "udp://" + defaultHost + ":" + port;
        fallbackWsUrl = "ws://"  + defaultHost + ":" + port;
        // A baked join key (handout builds; see constants.h) rides the URL the
        // same way a ?key= on a hand-typed URL arg does.
        std::string bakedKey = PLATFORMZ_DEFAULT_SERVER_KEY;
        if (!bakedKey.empty()) {
            serverUrl     += "?key=" + bakedKey;
            fallbackWsUrl += "?key=" + bakedKey;
        }
        autoFallback  = true;
    } else {
        networked = false;                             // dev default: no arg -> local single-player
    }
#endif
    // Join key: if the server was launched with PLATFORMZ_KEY, every join must
    // carry it. It travels inside the URL (?key=...) however that URL arrived -
    // invite link, command-line arg, or baked default. ws/wss servers read it
    // straight from the URL during the HTTP upgrade; UDP has no URL on the
    // wire, so extract it here and serializeHello carries it instead.
    std::string joinKey;
    {
        auto q = serverUrl.find('?');
        if (q != std::string::npos) {
            std::string query = serverUrl.substr(q + 1);
            size_t k = 0;
            while (k != std::string::npos) {
                if (query.compare(k, 4, "key=") == 0) {
                    joinKey = query.substr(k + 4, query.find('&', k) - (k + 4));
                    break;
                }
                k = query.find('&', k);
                if (k != std::string::npos) ++k;
            }
        }
    }
//MARK: SETUP
    // --- Setup (runs once) ---
    const int screenWidth = 1000;
    const int screenHeight = 700;
    const int textHeight = 20;
    InitWindow(screenWidth, screenHeight, "PLATFORMZ");
    // Assets load by relative path (assets/sounds, assets/music), so anchor the
    // working directory to the binary's own folder. Without this, launching the
    // executable from anywhere else (double-clicking it in Finder runs it from
    // ~) makes every asset load fail and the game runs fully silent. On the
    // web build GetApplicationDirectory() is "/", where the preloaded FS lives.
    ChangeDirectory(GetApplicationDirectory());
    SetTargetFPS(60);
    SetExitKey(KEY_NULL); // Esc is ours (free/recapture the mouse), not raylib's
                          // quit key - quit via the window close button or Cmd+Q.
    EnableCursor(); // start on the TITLE screen with a free cursor; the mouse is
                    // captured on entry to PLAYING (startGame) and freed again on
                    // the menu screens (returnToTitle / game-over trigger)

    // MARK: AUDIO FILES
    // raylib's mixer must be up before any sound loads. The fxTable maps each
    // AudioFXId (the shared client/server wire id) to a loaded sound; game
    // events arrive as ids (locally or over the network) and index into it.
    InitAudioDevice();
    SetMasterVolume(1.0f);

    // Per-FX voice pools live inside audioFX. Columns:
    //   volume, localOnly, spacial, poolSize, pitchJitter
    audioFX fxTable[FX_COUNT] = {
        audioFX("assets/sounds/rocket_launch.wav",  0.5f, false, true,  8, 0.08f),     // FX_ROCKET_LAUNCH
        audioFX("assets/sounds/explosion.wav",      1.0f, false, true,  8, 0.08f),     // FX_EXPLOSION
        audioFX("assets/sounds/asteroid_bonus.wav", 1.0f, true,  false, 2),            // FX_ASTEROID_BONUS
        audioFX("assets/sounds/player_hit.wav",     1.0f, true,  false, 2),            // FX_PLAYER_HIT (local only)
        audioFX("assets/sounds/player_death.wav",   1.0f, true,  false, 1),            // FX_PLAYER_DEATH (local only)
        audioFX("assets/sounds/no_ammo.wav",        1.0f, true,  false, 1),            // FX_NO_AMMO
        audioFX("assets/sounds/no_fuel.wav",        0.25f, true,  false, 1),            // FX_NO_FUEL
        audioFX("assets/sounds/firerate_choke.wav", 1.0f, true,  false, 1),            // FX_FIRERATE_CHOKE
        audioFX("assets/sounds/wall_bounce_player.wav", 0.6f, false, true, 2),         // FX_WALL_BOUNCE_PLAYER (spatial)
        audioFX("assets/sounds/rocket_through_wall.wav", 1.0f, false, true, 4),        // FX_ROCKET_THROUGH_WALL (spatial)
        audioFX({ "assets/sounds/move_through_platform_0.wav",
                  "assets/sounds/move_through_platform_1.wav",
                  "assets/sounds/move_through_platform_2.wav",
                  "assets/sounds/move_through_platform_3.wav" },
                0.5f, true, false, 0.08f),                                             // FX_PLATFORM_PASSTHROUGH (4 round-robin variation files, gated below)
        audioFX("assets/sounds/player_elimination_score.wav", 1.0f, true, false, 2),   // FX_PLAYER_ELIMINATION_SCORE (local only)
        audioFX("assets/sounds/player_local_damage.wav", 1.0f, true, false, 3, 0.08f), // FX_PLAYER_LOCAL_DAMAGE (local only)
        audioFX("assets/sounds/warning.wav",             0.4f, true, false, 2),       // FX_WARNING (local only)
        audioFX("assets/sounds/engage_earth_grav.wav",   0.8f, true, false, 1)         // FX_ENGAGE_EARTH_GRAVITY
    };
    // Platform passthrough is suppressed while earth-gravity engage is ringing
    // (one-directional: engage is never blocked, and passthrough's own 4
    // variation files still round-robin freely).
    fxTable[FX_PLATFORM_PASSTHROUGH].blockedBy = &fxTable[FX_ENGAGE_EARTH_GRAVITY];
    for (audioFX& fx : fxTable) fx.load();

    // MARK: MUSIC FILES
    // Client side: one MusicCue per MusicId, in enum order. The client owns and
    // loads every cue; *which* one plays is decided by the per-screen jukeboxes
    // below (locally here; over the wire the server will send the MusicId).
    MusicCue musicCueTable[MUSIC_COUNT] = {
        // filename, volume, fadeInDuration, fadeOutDuration, loop, loopStart, loopEnd (-1.0f = end of file), num_of_loops (-1 = infinite)
        MusicCue("assets/music/title.ogg", DEFAULT_MUSIC_VOLUME, 0.4f, 1.0f, true, 0.0f, -1.0f, -1),         // MUSIC_TITLE
        MusicCue("assets/music/countdown.ogg", DEFAULT_MUSIC_VOLUME, 0.0f, 4.0f, true, 0.0f, -1.0f, -1),     // MUSIC_COUNTDOWN
        MusicCue("assets/music/gameplay.ogg", DEFAULT_MUSIC_VOLUME, 0.25f, 1.0f, true, 0.0f, -1.0f, -1),     // MUSIC_GAMEPLAY
        MusicCue("assets/music/placeholder1.ogg", DEFAULT_MUSIC_VOLUME, 0.25f, 1.0f, true, 0.0f, -1.0f, -1), // MUSIC_PLACEHOLDER1
        MusicCue("assets/music/placeholder2.ogg", DEFAULT_MUSIC_VOLUME, 0.25f, 1.0f, true, 0.0f, -1.0f, -1), // MUSIC_PLACEHOLDER2
        MusicCue("assets/music/gameover.ogg", DEFAULT_MUSIC_VOLUME, 0.4f, 1.5f, true, 0.0f, -1.0f, -1)       // MUSIC_GAMEOVER
    };
    // load all cues
    for (MusicCue& mc : musicCueTable) mc.load();

    // Server side (this process, in local play): one jukebox per screen holds
    // that screen's MusicIds and picks the current one. Persists across games
    // so multi-track screens cycle through their list.
    Jukebox jukebox[SCREEN_COUNT];
    jukebox[SCREEN_TITLE].addTrack(MUSIC_TITLE);
    jukebox[SCREEN_COUNTDOWN].addTrack(MUSIC_COUNTDOWN);
    jukebox[SCREEN_GAMEPLAY].addTrack(MUSIC_GAMEPLAY);
    jukebox[SCREEN_GAMEPLAY].addTrack(MUSIC_PLACEHOLDER1);
    jukebox[SCREEN_GAMEPLAY].addTrack(MUSIC_PLACEHOLDER2);
    jukebox[SCREEN_GAMEPLAY].shuffle();
    jukebox[SCREEN_GAMEOVER].addTrack(MUSIC_GAMEOVER);

    // The in-loop music switch only fires on a screen *transition*, and the
    // game boots already on the title screen - start the first cue by hand.
    musicCueTable[jukebox[SCREEN_TITLE].getCurrentTrack()].fadeIn();


    // MARK: RENDER TEXTURE 2D
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

    MessageQueue messageQueue; // queue of messages to display after all state updates.

    // --- Networking (networked mode only) ---
    NetClient net;
    int       myIndex   = -1;     // our player slot, from the server's welcome packet
    uint32_t  inputSeq  = 0;      // monotonically increasing input sequence number
    // Handshake/reconnect bookkeeping (networked): resend hello until welcomed, and
    // (UDP only) treat a long state silence as a dropped connection so the handshake
    // runs again. Both in GetTime() secs. The silence-reset is gated to UDP because
    // WebSocket reports its own drops - on that transport a mere frame stall (an
    // unfocused window) isn't a disconnect, and resetting would flash the connecting
    // screen for no reason.
    bool      udpTransport = networked && serverUrl.rfind("udp://", 0) == 0; // not const: flips to false if the UDP->WS auto-fallback fires
    double    connectStartTime = 0.0; // GetTime() when we first connected; drives the UDP->WS fallback timeout
    double    lastHelloTime = 0.0;
    double    lastStateTime = 0.0;
    double    lastKeepaliveTime = 0.0;
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

    // MARK: BOT CONTROLLER
    // Owns the behaviour tree + per-slot decision/personality state and the
    // spawn/drive glue (see bot_controller.h). Shared with the server so local
    // and networked bots are built identically. LOCAL MODE marks slots 1+ as
    // bots in startGame(); networked mode takes isBot from the server.
    BotController botController;


    // MARK: SCREEN STATE
    // The loop runs in one of three screens. TITLE and GAME_OVER are simple
    // placeholder overlays handled at the top of the loop; PLAYING is the
    // original game body (update -> draw). Play is gated by TITLE and ends back
    // there via GAME_OVER, so the world is set up on each PLAYING entry (not
    // before the loop) and torn down on the way back to the title.
    enum class GameScreen { TITLE, COUNTDOWN, PLAYING, GAME_OVER };
    GameScreen screen = GameScreen::TITLE;
    float gameOverTimer = GAME_OVER_TIMER; // seconds since the last player died, to delay the GAME_OVER screen so the player sees the death FX
    float countdownRemaining = 0.0f; // local mode: seconds left in the pre-match "GAME STARTING IN..." countdown (world built but frozen)

    //MARK: Perf overlay
    // F3 toggles a frame-time overlay (FPS, avg/p95/max over the last 120 frames,
    // live object counts). While it's on, a greppable "PERF ..." line prints to
    // stdout every 5s - the paper trail for map-size benchmark runs.
    bool   perfOverlay = false;
    float  perfFrames[120] = {0};
    int    perfIdx = 0, perfCount = 0;
    double perfLastLog = 0.0;
    float netCountdown       = 0.0f; // networked: latest countdown-seconds-left the server sent (drives the same screen)
    bool  netMatchOver  = false; // networked: latched once the server reports the match ended, so the gameOverTimer countdown survives packet-less frames

    // Title-screen menu state (widgets live in ui.h). The name is local-only for
    // now (input + visuals); syncing it over the network is a later step.
    std::string playerName  = "PLAYER";
    bool        nameFocused = true;  // text field owns keyboard focus on entry (type without a click)
    bool        namePristine = true; // still the untouched "PLAYER" default; first keystroke clears it
    bool        showControls = false; // controls popup is open
    bool        showOptions  = false; // options popup is open

    // OPTIONS (see the OPTIONS modal below): one MatchOptions bundle drives the
    // sim (locally applied via GameSpace::applyOptions, remotely threaded
    // through serializeStart/serializeOptions). Drag latches guard the sliders
    // from server echoes while the mouse is on them.
    MatchOptions opt;
    // Random (non-repeating) order in which bot slots draw from BOT_NAME_STRINGS.
    // Seeded now so the first title screen is already randomized; re-rolled on
    // every return to the title screen so each match gets a fresh set of names.
    std::vector<int> botNameOrder = ShuffledIndices(BOT_NAME_COUNT);
    bool sliderPlayersActive = false; // drag latch: NUMBER OF PLAYERS
    bool sliderDiffActive    = false; // drag latch: BOT DIFFICULTY
    bool sliderWElastActive  = false; // drag latch: WALL ELASTICITY
    bool sliderPElastActive  = false; // drag latch: PLATFORM ELASTICITY
    bool sliderBoostActive   = false; // drag latch: SPEED BOOST
    bool sliderRSpeedActive  = false; // drag latch: ROCKET VELOCITY
    bool sliderJThrustActive = false; // drag latch: JETPACK THRUST
    bool sliderFBurnActive   = false; // drag latch: FUEL CONSUMPTION
    bool sliderFRegenActive  = false; // drag latch: FUEL REGEN
    bool sliderXRadiusActive = false; // drag latch: EXPLOSION RADIUS
    // UiSlider needs a float&; NUMBER OF PLAYERS and the two fuel controls are
    // ints in MatchOptions, so they get float shadows here, synced into
    // opt.numPlayers/fuelConsumption/fuelRegenPct on change (see the modal below).
    float optNumPlayersF = (float)opt.numPlayers;
    float optFuelBurnF   = (float)opt.fuelConsumption;
    float optFuelRegenF  = (float)opt.fuelRegenPct;
    // Networked options sync (lobby): the server echoes the match-wide options in
    // every state packet so any client's change shows live. Sliders are guarded
    // from server echoes while being dragged (their active latches); the toggles
    // have no such latch, so we remember the value we last sent and only accept
    // a server toggle that differs from it - that keeps our own click from being
    // flipped back before the server's echo of it arrives.
    bool  optSentWalls = opt.wallsEnabled;
    bool  optSentPhys  = opt.rocketsObeyPhysics;
    bool  optSentFf    = opt.friendlyFire;

    // Networked client connects once, on launch: the title screen then acts as a
    // live lobby (the server owns the world and only starts it on request). Local
    // mode has no server.
    if (networked) {
        net.connect(serverUrl); // non-blocking; IXWebSocket retries on its own thread
        connectStartTime = GetTime(); // start the UDP->WS fallback clock (InitWindow already ran)
        TraceLog(LOG_INFO, "Networked mode: connecting to %s", serverUrl.c_str());
    }

    // Our own display name for the roster/scoreboard. Once the user types a name
    // it wins; until then (namePristine) fall back to the slot-numbered default
    // "PLAYER {slot+1}" so two un-named humans don't both read "PLAYER". Networked
    // uses our server slot (myIndex); local mode is always slot 0 ("PLAYER 1").
    auto myDisplayName = [&]() -> std::string {
        if (!namePristine && !playerName.empty()) return playerName;
        int slot = networked ? (myIndex >= 0 ? myIndex : 0) : 0;
        return "PLAYER " + std::to_string(slot + 1);
    };

    // START from the title. Local: stand up a fresh run with the chosen map preset
    // (small/medium/large). Networked: ask the server to start/restart a match -
    // the screen flips to PLAYING when the server's phase says so (see the loop).
    auto startGame = [&](float halfSize, int platforms, int asteroids) {
        if (networked) {
            // Send the chosen map preset + the full OPTIONS bundle; the server
            // applies them before generating the world (first press wins).
            if (net.isOpen()) net.send(serializeStart(halfSize, platforms, asteroids, opt));
            return;
        }
        gameSpace.configureMap(halfSize, platforms, asteroids); // apply the chosen map preset
        gameSpace.applyOptions(opt); // OPTIONS: elasticities, speed/rocket/jetpack/explosion scales, fuel rates, gameplay toggles
        gameSpace.setPlayerCount(opt.numPlayers); // OPTIONS: 1 human + (N-1) bots
        gameSpace.generate(); // platforms, asteroids, and player slots
        // Local mode owns its sim: mark/color the wander-bot slots (index 1+).
        // (Networked mode takes isBot from the server over the wire instead.)
        std::vector<Player>& ps = gameSpace.getPlayers();
        // Slot 0 is the local human; carry the title-screen name onto it so the
        // scoreboard shows it (networked play gets this from the server instead).
        ps[0].name = myDisplayName();
        for (size_t i = 1; i < ps.size(); ++i) {
            ps[i].isBot = true;
            ps[i].color_outline = BOT_OUTLINE_COLOR;
            ps[i].color_fill    = BOT_FILL_COLOR;
            // This slot's shuffled name pick — same order the title-screen lobby
            // previews, so the roster shown before START matches the one in-game.
            ps[i].name = BOT_NAME_STRINGS[botNameOrder[(i - 1) % BOT_NAME_COUNT]];
        }
        // Size per-slot bot state and seed personalities (deterministic from each
        // slot's id, so the same map replays the same bots).
        botController.init(ps, opt.botDifficulty);
        gameOverTimer = GAME_OVER_TIMER; // fresh game-over countdown for this run
        // World is built but stays frozen: enter the pre-match countdown instead of
        // PLAYING. The COUNTDOWN block below ticks the timer and flips to PLAYING at
        // zero (capturing the cursor then). Cursor stays free during the count.
        countdownRemaining = COUNTDOWN_SECONDS;
        screen = GameScreen::COUNTDOWN;
    };

    // Networked: the server's phase just went PLAYING (we started, a peer started,
    // or we joined a running match). Reset per-match prediction/flash state (NOT
    // inputSeq - it stays monotonic across matches so the server doesn't discard
    // our packets) and enter the match. myIndex persists.
    auto enterNetworkedMatch = [&]() {
        prevHealth = -1; netHurt = 0.0f;
        netMatchOver = false; gameOverTimer = GAME_OVER_TIMER; // fresh game-over countdown for this match
        // Start the match with clean feeds: the game-over -> countdown ->
        // playing route never passes through returnToTitle, so clear here too
        // (covers leftovers from the tail end of the previous match).
        audioQueue.clearAll();
        messageQueue.clearAll();
        showControls = false; showOptions = false; // close any open lobby modal so it can't hold the freed cursor
        DisableCursor();
        consumeFirstLook = true; // swallow the cursor-lock delta on the first play frame
        consumeFirstFire = true; // swallow the start click so it isn't read as a rocket
        // Seed the look prediction from the server's spawn orientation NOW - the
        // countdown states already carried it into our slot - so the very first
        // PLAYING frame sends and renders the correct facing instead of a stale
        // predYaw (which showed the 2nd player facing the wrong way at spawn until
        // they moved). If our slot isn't known yet, fall back to the lazy seed in
        // the PLAYING block.
        std::vector<Player>& ps = gameSpace.getPlayers();
        if (myIndex >= 0 && myIndex < (int)ps.size()) {
            predYaw = ps[myIndex].yaw; predPitch = ps[myIndex].pitch; predInit = true;
        } else {
            predInit = false;
        }
        screen = GameScreen::PLAYING;
    };

    // Networked: drain server frames (apply state, track our slot), returning the
    // latest match phase. Used by the lobby/game-over screens, which otherwise
    // wouldn't poll the socket, so they can react to phase changes.
    auto pumpNet = [&]() -> ServerMessage::Phase {
        ServerMessage::Phase phase = ServerMessage::Phase::Unknown;
        for (const std::string& frame : net.poll()) {
            lastStateTime = GetTime(); // any server frame = the connection is alive
            ServerMessage m = applyMessage(frame, gameSpace);
            if (m.type == ServerMessage::Type::Welcome) {
                myIndex = m.playerId;
                // Now that we know our real slot, assert our name: send our display
                // name (custom, or the correct "PLAYER {slot+1}" default). The
                // server slot may carry a leftover lobby bot name, and pre-welcome
                // (myIndex == -1) every client's default would have been "PLAYER 1".
                net.send(serializeName(myDisplayName()));
            }
            else if (m.type == ServerMessage::Type::State) {
                phase = m.phase; netCountdown = m.countdown;
                // Apply the server's live options to our OPTIONS modal. Don't
                // stomp a control the local user is actively driving: skip a
                // slider while it's being dragged, and for the toggles only take
                // a server value that differs from the one we last sent (so our
                // own click isn't flipped back before its echo returns).
                if (m.hasOptions) {
                    // Update our OPTIONS modal, per-slider guarded by its drag
                    // latch so a control we're actively dragging isn't stomped.
                    if (!sliderPlayersActive) { opt.numPlayers = m.opt.numPlayers; optNumPlayersF = (float)opt.numPlayers; }
                    if (!sliderDiffActive)    opt.botDifficulty      = m.opt.botDifficulty;
                    if (!sliderWElastActive)  opt.wallElasticity     = m.opt.wallElasticity;
                    if (!sliderPElastActive)  opt.platformElasticity = m.opt.platformElasticity;
                    if (!sliderBoostActive)   opt.speedBoost         = m.opt.speedBoost;
                    if (!sliderRSpeedActive)  opt.rocketSpeedScale   = m.opt.rocketSpeedScale;
                    if (!sliderXRadiusActive) opt.explosionRadiusScale = m.opt.explosionRadiusScale;
                    if (!sliderJThrustActive) opt.jetpackThrust      = m.opt.jetpackThrust;
                    if (!sliderFBurnActive)  { opt.fuelConsumption = m.opt.fuelConsumption; optFuelBurnF  = (float)opt.fuelConsumption; }
                    if (!sliderFRegenActive) { opt.fuelRegenPct    = m.opt.fuelRegenPct;    optFuelRegenF = (float)opt.fuelRegenPct; }

                    // Mirror the server's full options bundle onto our gameSpace
                    // regardless of drag state (locally startGame does this) -
                    // networked draw/sim runs off OUR gameSpace, so an open-space
                    // match must render open, a bouncy wall must bounce, etc, even
                    // mid-drag on an unrelated slider.
                    gameSpace.applyOptions(m.opt);

                    if (m.opt.wallsEnabled       != optSentWalls) { opt.wallsEnabled = m.opt.wallsEnabled; optSentWalls = m.opt.wallsEnabled; }
                    if (m.opt.rocketsObeyPhysics != optSentPhys)  { opt.rocketsObeyPhysics = m.opt.rocketsObeyPhysics; optSentPhys = m.opt.rocketsObeyPhysics; }
                    if (m.opt.friendlyFire       != optSentFf)    { opt.friendlyFire = m.opt.friendlyFire; optSentFf = m.opt.friendlyFire; }
                }
            }
        }
        // The lobby/countdown/game-over screens that call pumpNet all `continue`
        // before the PLAYING body's drains, so the kill-feed messages and audio
        // events each state packet APPENDS to gameSpace would otherwise pile up
        // for as long as those screens are shown (the server keeps simulating
        // the bot fight through GAMEOVER) - then burst-replay on the first
        // frame of the next match. These screens ignore the event feed by
        // design, so drop it at the source.
        gameSpace.getMessages().clear();
        gameSpace.getAudioEvents().clear();
        return phase;
    };

    // GAME_OVER/title -> TITLE. Local: wipe the world for a clean restart.
    // Networked: stay connected (back to the lobby) so START can restart; the
    // server owns the world and resyncs it. The NetClient dtor closes on exit.
    auto returnToTitle = [&]() {
        if (!networked) gameSpace.clear();
        // Drop any not-yet-played sounds and still-live kill-feed entries from
        // the match we just left, so they can't replay on the next one.
        audioQueue.clearAll();
        messageQueue.clearAll();
        botNameOrder = ShuffledIndices(BOT_NAME_COUNT); // fresh random bot names next match
        EnableCursor(); // free the cursor for the title menu
        showControls = false; showOptions = false; // no stale modal flag leaking back onto the lobby
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

    // Remember the previous screen so we can detect a transition and change music.
    GameScreen previousScreen = screen;

    // Map the client's screen state onto the jukebox array's ScreenId index.
    auto screenIdOf = [](GameScreen s) {
        switch (s) {
            case GameScreen::TITLE:     return SCREEN_TITLE;
            case GameScreen::COUNTDOWN: return SCREEN_COUNTDOWN;
            case GameScreen::PLAYING:   return SCREEN_GAMEPLAY;
            case GameScreen::GAME_OVER: return SCREEN_GAMEOVER;
        }
        return SCREEN_TITLE;
    };

    //MARK: Bench launch
    // Skip the title and stand up the requested world immediately. Reuses the
    // exact production startGame path; interactive - you fly, the PERF lines
    // are the record.
    if (benchMode) {
        opt.numPlayers = benchPlayers; optNumPlayersF = (float)benchPlayers; // 1 human + (N-1) bots: realistic sim/rocket/spark load
        perfOverlay = true;
        SetTargetFPS(0); // uncap so frame times reflect true throughput, not vsync pacing
        startGame(benchHalf, benchPlat, benchRoid);
        countdownRemaining = 1.0f; // shorten the pre-match freeze
    }

    //MARK: MAIN LOOP
    // --- The loop itself ---
    while (!WindowShouldClose()) {  // true when user hits the window close button (Esc is repurposed below)

        // 1. TIME
        // dt = seconds since last frame. Multiply all movement by this
        // so speed is consistent regardless of framerate.
        float dt = GetFrameTime();

        // Perf ring: record every frame (cheap) so the F3 overlay has history
        // the moment it's toggled on. F3 types no character, so the title
        // screen's name field is unaffected.
        perfFrames[perfIdx] = dt;
        perfIdx = (perfIdx + 1) % 120;
        if (perfCount < 120) perfCount++;
        if (IsKeyPressed(KEY_F3)) perfOverlay = !perfOverlay;

        // MARK: MUSIC STREAM
        if (screen != previousScreen) {
            for (MusicCue& mc : musicCueTable) {
                if (mc.isPlaying()) mc.fadeOut();
            }
            // Advance the jukebox of the screen we're leaving so the next visit
            // gets a fresh track (no-op for single-track screens), then fade in
            // whatever the new screen's jukebox currently points at.
            jukebox[screenIdOf(previousScreen)].next();
            MusicId id = jukebox[screenIdOf(screen)].getCurrentTrack();
            if (id != MUSIC_COUNT) musicCueTable[id].fadeIn();
            previousScreen = screen;
        }
        for (MusicCue& mc : musicCueTable) mc.update(dt);

        // Connection maintenance (networked). Transport-agnostic, but it's what
        // makes the connectionless UDP path work: resend hello until the server
        // welcomes us (assigns a slot), and if a welcomed session goes silent
        // (UDP peer reaped, or a drop) forget our slot so the handshake re-runs.
        // Over WebSocket the server auto-welcomes on connect, so the hello is just
        // a harmless re-welcome and TCP keeps the session alive.
        if (networked) {
            double nowT = GetTime();
            // Auto-fallback (baked-in UDP default only): if the UDP handshake never
            // completes (no welcome, myIndex still -1) within the timeout, the path
            // is likely blocking UDP - switch once to WebSocket at the same host and
            // restart the handshake. WsTransport then retries on its own thread.
            if (autoFallback && udpTransport && myIndex < 0 && nowT - connectStartTime > 3.0) {
                TraceLog(LOG_WARNING, "UDP handshake timed out; falling back to WebSocket: %s",
                         fallbackWsUrl.c_str());
                net.connect(fallbackWsUrl); // swaps UdpTransport -> WsTransport (old socket closed by its dtor)
                udpTransport = false;       // stop UDP-only keepalive / silence-reset below
                autoFallback = false;       // one-shot
                connectStartTime = nowT;
                lastHelloTime = 0.0;        // send hello immediately on the new socket
            }
            if (net.isOpen() && myIndex < 0 && nowT - lastHelloTime > 0.5) {
                // Only carry a name if the user actually set one (same gate as
                // serializeName). Before welcome myIndex is -1, so myDisplayName()
                // would send the slot-0 default "PLAYER 1" for EVERY client and
                // clobber the server's correct per-slot default ("PLAYER 2", ...);
                // an empty name leaves the server's default in place.
                net.send(serializeHello(namePristine ? std::string() : playerName, joinKey));
                lastHelloTime = nowT;
            }
            // UDP keepalive: the client only streams input during PLAYING, so on
            // the lobby/countdown/game-over screens it would otherwise go silent
            // and the server's idle-reaper would free its slot mid-countdown. A
            // 1s heartbeat keeps the slot alive on every screen. UDP only - WS is
            // kept alive by TCP and is never reaped.
            if (udpTransport && myIndex >= 0 && nowT - lastKeepaliveTime > 1.0) {
                net.send(serializeKeepalive());
                lastKeepaliveTime = nowT;
            }
            if (udpTransport && myIndex >= 0 && lastStateTime > 0.0 && nowT - lastStateTime > 3.0)
                myIndex = -1; // UDP only: treat as disconnected; resume the hello handshake
        }

        // Web only: keep shell.html's pointer-lock handler in sync with whether a
        // title-screen modal is open (no-op on native). Modals live only on the
        // title screen, so force it false everywhere else.
        PlatformzSetModalOpen(screen == GameScreen::TITLE && (showControls || showOptions));

        // MARK: TITLE SCREEN
        // Placeholder front/end screens. Each handles its own input + draw and
        // skips the rest of the loop; PLAYING (below) is the original game body.
        if (screen == GameScreen::TITLE) {
            // Networked: the title screen is the lobby - pump the server so the
            // player list stays live and we follow the server into a match (we
            // started it, a peer did, or we joined one already running). The server
            // opens a match with the shared pre-match countdown; follow it there so
            // every client counts down together. If we joined mid-match we skip
            // straight to PLAYING.
            if (networked) {
                ServerMessage::Phase p = pumpNet();
                if (p == ServerMessage::Phase::Countdown) { screen = GameScreen::COUNTDOWN; continue; }
                if (p == ServerMessage::Phase::Playing)   { enterNetworkedMatch(); continue; }
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
                DrawStarfieldBackdrop((float)GetTime()); // slow-drifting stars behind the UI
                UiTextCentered("PLATFORMZ", screenWidth, 110, 80, RAYWHITE);

                // Name entry (local-only for now).
                UiTextCentered("NAME", screenWidth, 215, 20, ui::OUTLINE);
                Rectangle nameBox = {350, 240, 300, 40};
                // Let the name fill the space the UI allots but never overflow
                // it. The tightest renderer is the lobby roster row below:
                // "%d. NAME (YOU)" at font 18 inside the 300px players panel.
                // Convert its leftover width to the field's font size (20) and
                // let UiTextField reject chars past that budget.
                int nameBudget = (280 - MeasureText("8. ", 18) - MeasureText(" (YOU)", 18)) * 20 / 18;
                // Push every edit to the server so the latest typed name wins
                // (the welcome already sent a baseline before this field changed).
                if (UiTextField(nameBox, playerName, nameFocused, PLAYER_NAME_MAX_CHARS, 20,
                                &namePristine, nameBudget) &&
                    networked && net.isOpen())
                    net.send(serializeName(playerName));

                // Players panel. Local: GAMESPACE_NUMBER_OF_PLAYERS slots - slot 1
                // is the human, the rest bot-filled (local play spawns 1 human +
                // N-1 bots). Networked: the live lobby - connected slots from the
                // server (isConnected), yours marked (YOU). Panel height tracks the
                // row count.
                std::vector<Player>& titlePlayers = gameSpace.getPlayers();

                // Host identity: only "player 1" - the lowest connected HUMAN slot
                // - may adjust OPTIONS and START the match; everyone else sees a
                // passive lobby with a "waiting for the host" line. hostSlot is the
                // smallest human-occupied slot (-1 until the server tells us who's
                // connected); host migrates automatically if that client leaves.
                // Bot slots also carry isConnected (their bodies render in a match),
                // so exclude them with !isBot. Local single-player is always host.
                int hostSlot = -1;
                for (int i = 0; i < (int)titlePlayers.size(); ++i)
                    if (titlePlayers[i].isConnected && !titlePlayers[i].isBot) { hostSlot = i; break; }
                bool amHost = !networked || (myIndex >= 0 && myIndex == hostSlot);
                if (!amHost) showOptions = false; // never leave the OPTIONS modal open on a non-host (e.g. after a host handoff)

                // Networked: preview the roster the match will build - the
                // connected humans plus bot-fillers up to the chosen NUMBER OF
                // PLAYERS. The server holds a full slot set (unclaimed ones held by
                // bots, all flagged isConnected) and echoes opt.numPlayers to every
                // client, so this preview tracks the host's slider live on ALL
                // clients. Never hide a connected human sitting above the chosen
                // count (a mid-roster slot can be free while a higher one is taken).
                int rowsShown;
                int previewCount = 0; // networked: number of roster rows to draw (slots 0..previewCount-1)
                if (networked) {
                    int lastHumanSlot = -1;
                    for (int i = 0; i < (int)titlePlayers.size(); ++i)
                        if (titlePlayers[i].isConnected && !titlePlayers[i].isBot) lastHumanSlot = i;
                    previewCount = std::min(std::max(opt.numPlayers, lastHumanSlot + 1),
                                            (int)titlePlayers.size());
                    rowsShown = previewCount > 0 ? previewCount : 1; // >=1 so the "waiting" line has a row
                } else {
                    rowsShown = opt.numPlayers; // OPTIONS slider previews the roster
                }
                const float rowH = 24.0f, headerH = 30.0f;
                Rectangle playersBox = {350, 300, 300, headerH + rowsShown * rowH + 10.0f};
                UiPanel(playersBox);
                DrawText("PLAYERS", (int)playersBox.x + 10, (int)playersBox.y + 8, 14, ui::OUTLINE);
                if (networked) {
                    if (previewCount == 0) {
                        DrawText("Waiting for players...", (int)playersBox.x + 10,
                                 (int)(playersBox.y + headerH), 18, GRAY);
                    }
                    // Slots 0..previewCount-1 are all occupied (human or bot), so
                    // draw them as contiguous rows.
                    for (int i = 0; i < previewCount; ++i) {
                        int ry = (int)(playersBox.y + headerH + i * rowH);
                        bool you = (i == myIndex);
                        // Our row shows the live-typed name (or our slot-numbered
                        // default while untouched); other rows show the server-synced
                        // name, falling back to a slot label until they've set one.
                        std::string shown = you
                            ? myDisplayName()
                            : (titlePlayers[i].name.empty() ? TextFormat("PLAYER %d", i + 1)
                                                            : titlePlayers[i].name);
                        DrawText(TextFormat("%d. %s%s", i + 1, shown.c_str(), you ? " (YOU)" : ""),
                                 (int)playersBox.x + 10, ry, 18, you ? RAYWHITE : ui::OUTLINE);
                    }
                } else {
                    for (int i = 0; i < rowsShown; ++i) {
                        int ry = (int)(playersBox.y + headerH + i * rowH);
                        if (i == 0)
                            DrawText(TextFormat("1. %s (YOU)", myDisplayName().c_str()),
                                     (int)playersBox.x + 10, ry, 18, RAYWHITE);
                        else
                            DrawText(TextFormat("%d. %s", i + 1, BOT_NAME_STRINGS[botNameOrder[(i - 1) % BOT_NAME_COUNT]]),
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
                if (ready && amHost) {
                    Rectangle bs  = {110, startY, 180, 50};
                    Rectangle bm  = {310, startY, 180, 50};
                    Rectangle bl  = {510, startY, 180, 50};
                    Rectangle bxl = {710, startY, 180, 50};
                    if (uiEnabled && UiButton(bs, "SMALL"))  startGame(mapSizePresets["SMALL"].halfSize, mapSizePresets["SMALL"].numPlatforms, mapSizePresets["SMALL"].numAsteroids);
                    if (uiEnabled && UiButton(bm, "MEDIUM")) startGame(mapSizePresets["MEDIUM"].halfSize, mapSizePresets["MEDIUM"].numPlatforms, mapSizePresets["MEDIUM"].numAsteroids);
                    if (uiEnabled && UiButton(bl, "LARGE")) startGame(mapSizePresets["LARGE"].halfSize, mapSizePresets["LARGE"].numPlatforms, mapSizePresets["LARGE"].numAsteroids);
                    if (uiEnabled && UiButton(bxl, "XL")) startGame(mapSizePresets["XL"].halfSize, mapSizePresets["XL"].numPlatforms, mapSizePresets["XL"].numAsteroids);
                } else if (!ready) {
                    UiTextCentered(myIndex >= 0 ? "JOINING..." : "CONNECTING...",
                                   screenWidth, (int)startY + 14, 20, GRAY);
                } else {
                    // Connected but not the host: only "player 1" starts the match.
                    // Show who we're waiting on (their synced name, or the slot-
                    // numbered default until they've set one - same fallback as the
                    // roster rows above).
                    std::string hostName = (hostSlot >= 0 && !titlePlayers[hostSlot].name.empty())
                        ? titlePlayers[hostSlot].name
                        : TextFormat("PLAYER %d", hostSlot + 1);
                    UiTextCentered(TextFormat("Waiting for %s to start the game.", hostName.c_str()),
                                   screenWidth, (int)startY + 14, 20, GRAY);
                }

                // Opening a modal must leave the cursor free (the popups are
                // click-driven, especially OPTIONS). The title screen is already a
                // free-cursor state, but guard explicitly so this holds if a modal
                // is ever opened from a captured (in-game) context.
                Rectangle controlsBtn = {400, startY + 64.0f, 200, 44};
                if (uiEnabled && UiButton(controlsBtn, "CONTROLS")) { showControls = true; if (IsCursorHidden()) EnableCursor(); }
                // OPTIONS is host-only (it reconfigures the whole match); non-hosts
                // don't get the button, matching the START gating above.
                if (amHost) {
                    Rectangle optionsBtn = {400, startY + 116.0f, 200, 44};
                    if (uiEnabled && UiButton(optionsBtn, "OPTIONS")) { showOptions = true; if (IsCursorHidden()) EnableCursor(); }
                }

                // Controls popup, drawn last so it sits on top. Opaque panel
                // (UiModalPanel) so the dimmed title UI doesn't bleed through.
                if (showControls) {
                    Rectangle m = {250, 170, 500, 360};
                    UiModalChrome(m, "CONTROLS");
                    const char* lines[] = {
                        "WASD          move",
                        "Mouse         look",
                        "Left click    fire rocket",
                        "Space         jetpack (up)",
                        "Left Shift    stronger (earth) gravity",
                        "M             end match (player 1 / host only)",
                        "Esc           toggle cursor capture",
                    };
                    int ly = (int)m.y + 80;
                    for (const char* ln : lines) { DrawText(ln, (int)m.x + 40, ly, 18, RAYWHITE); ly += 34; }
                    if (UiModalClose(m, controlsWasOpen)) showControls = false;
                }

                // Options popup, same opaque modal style. Two columns of five
                // sliders (match size/difficulty, elasticities, speed/rocket/
                // jetpack/explosion multipliers, fuel rates) plus a row of three
                // gameplay toggles; they drive local play directly and ride the
                // start request to the server for networked play. Wide + raised
                // so both columns and the toggle row fit the 700px window.
                if (showOptions) {
                    Rectangle m = {110, 10, 780, 680}; // two 5-slider columns + toggle row + CLOSE
                    UiModalChrome(m, "OPTIONS");

                    const float colW = 330.0f, gutter = 40.0f;
                    float lxL = m.x + 40, lxR = lxL + colW + gutter;
                    // Right-aligned value readout next to each label, within
                    // the given column's right edge.
                    auto valueAt = [&](const char* v, float colX, int y) {
                        int vw = MeasureText(v, 18);
                        DrawText(v, (int)(colX + colW - vw), y, 18, ui::OUTLINE);
                    };

                    // Each control returns true the frame it changes; in networked
                    // play we push the new option set to the server so every client's
                    // modal updates live (mirrors the name-field sync above).
                    bool optChanged = false;

                    int y1 = (int)m.y + 80, y2 = y1 + 85, y3 = y2 + 85, y4 = y3 + 85, y5 = y4 + 85;

                    // --- Left column ---
                    // NUMBER OF PLAYERS (integer, 1..GAMESPACE_NUMBER_OF_PLAYERS).
                    DrawText("NUMBER OF PLAYERS", (int)lxL, y1, 18, RAYWHITE);
                    valueAt(TextFormat("%d", (int)optNumPlayersF), lxL, y1);
                    if (UiSlider({lxL, (float)(y1 + 26), colW, 22}, optNumPlayersF,
                             1.0f, (float)GAMESPACE_NUMBER_OF_PLAYERS,
                             sliderPlayersActive, 1.0f)) {
                        opt.numPlayers = (int)optNumPlayersF; optChanged = true;
                    }

                    // BOT DIFFICULTY (continuous, 0.0..BOT_DIFFICULTY).
                    DrawText("BOT DIFFICULTY", (int)lxL, y2, 18, RAYWHITE);
                    valueAt(TextFormat("%.2f", opt.botDifficulty), lxL, y2);
                    if (UiSlider({lxL, (float)(y2 + 26), colW, 22}, opt.botDifficulty,
                             0.0f, BOT_DIFFICULTY, sliderDiffActive)) optChanged = true;

                    // WALL ELASTICITY (players only; asteroids keep their constant).
                    DrawText("WALL ELASTICITY", (int)lxL, y3, 18, RAYWHITE);
                    valueAt(TextFormat("%.2f", opt.wallElasticity), lxL, y3);
                    if (UiSlider({lxL, (float)(y3 + 26), colW, 22}, opt.wallElasticity,
                             0.0f, 1.0f, sliderWElastActive)) optChanged = true;

                    // PLATFORM ELASTICITY (players only; asteroids keep their constant).
                    DrawText("PLATFORM ELASTICITY", (int)lxL, y4, 18, RAYWHITE);
                    valueAt(TextFormat("%.2f", opt.platformElasticity), lxL, y4);
                    if (UiSlider({lxL, (float)(y4 + 26), colW, 22}, opt.platformElasticity,
                             0.0f, 1.0f, sliderPElastActive)) optChanged = true;

                    // SPEED BOOST (walk + jetpack speed/accel, and rocket speed).
                    DrawText("SPEED BOOST", (int)lxL, y5, 18, RAYWHITE);
                    valueAt(TextFormat("%.1fx", opt.speedBoost), lxL, y5);
                    if (UiSlider({lxL, (float)(y5 + 26), colW, 22}, opt.speedBoost,
                             1.0f, 2.0f, sliderBoostActive)) optChanged = true;

                    // --- Right column ---
                    // ROCKET VELOCITY (on top of SPEED BOOST).
                    DrawText("ROCKET VELOCITY", (int)lxR, y1, 18, RAYWHITE);
                    valueAt(TextFormat("%.1fx", opt.rocketSpeedScale), lxR, y1);
                    if (UiSlider({lxR, (float)(y1 + 26), colW, 22}, opt.rocketSpeedScale,
                             1.0f, 2.0f, sliderRSpeedActive)) optChanged = true;

                    // JETPACK THRUST (on top of SPEED BOOST; jetpack only).
                    DrawText("JETPACK THRUST", (int)lxR, y2, 18, RAYWHITE);
                    valueAt(TextFormat("%.1fx", opt.jetpackThrust), lxR, y2);
                    if (UiSlider({lxR, (float)(y2 + 26), colW, 22}, opt.jetpackThrust,
                             1.0f, 2.0f, sliderJThrustActive)) optChanged = true;

                    // FUEL CONSUMPTION (direct units/sec out of the 100-unit tank).
                    DrawText("FUEL CONSUMPTION", (int)lxR, y3, 18, RAYWHITE);
                    valueAt(TextFormat("%d/sec", (int)optFuelBurnF), lxR, y3);
                    if (UiSlider({lxR, (float)(y3 + 26), colW, 22}, optFuelBurnF,
                             0.0f, 100.0f, sliderFBurnActive, 1.0f)) {
                        opt.fuelConsumption = (int)optFuelBurnF; optChanged = true;
                    }

                    // FUEL REGEN (percentage of the consumption rate; 100% = keeps pace).
                    DrawText("FUEL REGEN", (int)lxR, y4, 18, RAYWHITE);
                    valueAt(TextFormat("%d%%", (int)optFuelRegenF), lxR, y4);
                    if (UiSlider({lxR, (float)(y4 + 26), colW, 22}, optFuelRegenF,
                             0.0f, 100.0f, sliderFRegenActive, 1.0f)) {
                        opt.fuelRegenPct = (int)optFuelRegenF; optChanged = true;
                    }

                    // EXPLOSION RADIUS (damage radius + blast visual; last in the
                    // modal per its own domain, grouped away from the speed trio).
                    DrawText("EXPLOSION RADIUS", (int)lxR, y5, 18, RAYWHITE);
                    valueAt(TextFormat("%.1fx", opt.explosionRadiusScale), lxR, y5);
                    if (UiSlider({lxR, (float)(y5 + 26), colW, 22}, opt.explosionRadiusScale,
                             1.0f, 4.0f, sliderXRadiusActive)) optChanged = true;

                    // Toggles: three across, label on its own line, a compact ON/OFF
                    // control below (labels are long, so keep them off the control's
                    // line). Each defaults to its constants.h value; applied at match
                    // start. Sliders use an 85px rhythm; this row sits just below them.
                    // Three explicit x positions (not the slider columns) so the
                    // widest label - ROCKETS OBEY PHYSICS, ~230px at font 18 -
                    // clears its neighbors on both sides.
                    int y6 = y5 + 85;
                    float txBoundary = lxL;        // 150
                    float txPhysics  = lxL + 190.0f; // 340
                    float txFriendly = lxL + 450.0f; // 600

                    DrawText("BOUNDARY WALLS", (int)txBoundary, y6, 18, RAYWHITE);
                    if (UiToggle({txBoundary, (float)(y6 + 26), 100, 24}, opt.wallsEnabled)) {
                        optChanged = true; optSentWalls = opt.wallsEnabled;
                    }

                    DrawText("ROCKETS OBEY PHYSICS", (int)txPhysics, y6, 18, RAYWHITE);
                    if (UiToggle({txPhysics, (float)(y6 + 26), 100, 24}, opt.rocketsObeyPhysics)) {
                        optChanged = true; optSentPhys = opt.rocketsObeyPhysics;
                    }

                    DrawText("FRIENDLY FIRE", (int)txFriendly, y6, 18, RAYWHITE);
                    if (UiToggle({txFriendly, (float)(y6 + 26), 100, 24}, opt.friendlyFire)) {
                        optChanged = true; optSentFf = opt.friendlyFire;
                    }

                    // Push the change to the server (it re-broadcasts to all clients).
                    if (optChanged && networked && net.isOpen())
                        net.send(serializeOptions(opt));

                    if (UiModalClose(m, optionsWasOpen)) showOptions = false;
                }
            EndDrawing();
            continue;
        }
        // MARK: COUNTDOWN SCREEN
        // Pre-match "GAME STARTING IN..." hold. The world is already built but
        // frozen (this block continues before the PLAYING sim body). Local mode
        // owns the timer; networked mode follows the server's countdown so every
        // client counts down together and enters on the same tick.
        if (screen == GameScreen::COUNTDOWN) {
            float remaining;
            if (networked) {
                ServerMessage::Phase p = pumpNet();
                if (p == ServerMessage::Phase::Playing) { enterNetworkedMatch(); continue; }
                // Match ended/canceled before it began -> back to the lobby.
                if (p == ServerMessage::Phase::Lobby || p == ServerMessage::Phase::GameOver) { returnToTitle(); continue; }
                remaining = netCountdown;
            } else {
                countdownRemaining -= dt;
                if (countdownRemaining <= 0.0f) { // count reached zero: unfreeze into the match
                    DisableCursor(); // capture the mouse for free-look while playing
                    consumeFirstLook = true; // swallow the cursor-lock delta on the first play frame
                    consumeFirstFire = true; // swallow the start click so it isn't read as a rocket
                    screen = GameScreen::PLAYING;
                    continue;
                }
                remaining = countdownRemaining;
            }

            // Helper lines fade in one after another across the count. elapsed is the
            // same 0..COUNTDOWN_SECONDS ramp whether driven by the local or server
            // clock, so the staggered reveal is identical in both modes.
            float elapsed = COUNTDOWN_SECONDS - remaining;
            struct { const char* text; float start; } infoLines[] = {
                { "Eliminate other players to win.",                      0.0f },
                { "Destroy asteroids to replenish ammo, fuel, and health.", COUNTDOWN_SECONDS * 0.34f },
                { "Good luck!",                                          COUNTDOWN_SECONDS * 0.68f },
            };
            // Adjust the info for a single player game:
            if (gameSpace.getPlayers().size() == 1) {
                infoLines[0].text = "Destroy every asteroid to win.";
                infoLines[1].text = "Each elimination replenishes ammo, fuel, and health.";
            }
            int countNum = (int)ceilf(remaining);
            if (countNum < 1) countNum = 1; // never flash "0" before the flip to PLAYING

            BeginDrawing();
                ClearBackground(BLACK);
                DrawStarfieldBackdrop((float)GetTime()); // slow-drifting stars behind the UI
                UiTextCentered("PLATFORMZ", screenWidth, 110, 80, RAYWHITE); // keep the title
                UiTextCentered("GAME STARTING IN...", screenWidth, 260, 30, ui::OUTLINE);
                UiTextCentered(TextFormat("%d", countNum), screenWidth, 300, 90, RAYWHITE);
                int ly = 480;
                for (const auto& L : infoLines) {
                    float a = Clamp((elapsed - L.start) / 0.6f, 0.0f, 1.0f); // 0.6s fade-in per line
                    DrawCentered(L.text, ly, 24, Fade({0, 255, 200, 255}, a));  // platform color.
                    ly += 40;
                }
            EndDrawing();
            continue;
        }
        // MARK: GAME_OVER SCREEN
        if (screen == GameScreen::GAME_OVER) {
            // Networked: keep pumping the server. If a match (re)starts (a peer
            // pressed START, or we did after returning to the lobby), follow it in -
            // into the shared countdown first, or straight to PLAYING if we caught it
            // already running.
            if (networked) {
                ServerMessage::Phase p = pumpNet();
                if (p == ServerMessage::Phase::Countdown) { screen = GameScreen::COUNTDOWN; continue; }
                if (p == ServerMessage::Phase::Playing)   { enterNetworkedMatch(); continue; }
            }
            if (startPressed()) returnToTitle();
            BeginDrawing();
            ClearBackground(BLACK);
            // Stars behind the scoreboard. On the eliminated path the frozen
            // greyscale frame blitted below covers this (and already has stars
            // baked in from the capture pass) - unconditional is simplest.
            DrawStarfieldBackdrop((float)GetTime());

            Color scoreColor = WHITE;
            Color pressKeyColor = RED;
            std::vector<Player>& players = gameSpace.getPlayers();
            int localIndex = networked ? myIndex : 0;
            // Outcome banner for the local player.
            if (localIndex >= 0 && localIndex < (int)players.size()) {
                if (players[localIndex].isAlive) {
                    DrawCentered("GAME OVER", 240, 80, BLUE);
                    DrawCentered("You survived!", 360, 20, BLUE);
                    scoreColor = GRAY;
                    pressKeyColor = BLUE;
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
            DrawCentered("Press any key to return to title.", 440 + scoreRow * 20 + 10, 20, pressKeyColor);
            
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
            if (!IsCursorHidden()) {
                in = PlayerInput{}; // cursor free (Esc, or window unfocused): no look/move/fire
            } else {
                // First frame the mouse is actually captured: swallow the cursor-
                // centering jump and the match-start click HERE, not on the first
                // PLAYING frame - which can arrive before the window is focused (two
                // clients on one machine, or an alt-tab during the countdown), which
                // would otherwise leak the capture jump into the aim later.
                if (consumeFirstLook) { in.lookDelta = {0, 0}; consumeFirstLook = false; }
                if (consumeFirstFire) { in.fire = false; consumeFirstFire = false; }
            }
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
                lastStateTime = GetTime(); // any server frame = the connection is alive
                ServerMessage m = applyMessage(frame, gameSpace);
                if (m.type == ServerMessage::Type::Welcome) {
                    myIndex = m.playerId;
                    net.send(serializeName(myDisplayName())); // assert our name (see the pumpNet welcome)
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
                    audioQueue.push(fxTable[hp > 0 ? FX_PLAYER_LOCAL_DAMAGE : FX_PLAYER_DEATH],
                                    localPlayer->position, /*isLocal=*/true);
                }
                prevHealth = hp;
            }
            netHurt = fmaxf(0.0f, netHurt - dt / 0.5f); // decay over flash_duration (0.5s)

            // Dead-reckon between server snapshots: advance every mover by its
            // synced velocity so motion doesn't freeze on a gap frame and jump on
            // a double frame (the "stepped" look). The next snapshot re-bases
            // position, so this only smooths - it never drifts. Runs after the
            // snapshot is applied and before reticles/camera read positions.
            gameSpace.extrapolate(dt);

            // Color each player slot by its index, matching GameSpace::spawnPlayers():
            // bots are magenta (BOT_*_COLOR), humans round-robin through HUMAN_PLAYER_COLORS.
            // Color isn't sent over the wire (it's static per slot); the `bot` flag is.
            for (int i = 0; i < (int)players.size(); ++i)
                assignPlayerColor(players[i], i);
            // Tick reticles: snap ours, smooth everyone else's.
            for (int i = 0; i < (int)players.size(); ++i)
                players[i].updateReticle(dt, i != localIndex);
        } else {
            // --- LOCAL SINGLE-PLAYER: host the sim (original game loop) ---
            Player& player = gameSpace.getPlayers()[0];

            // Gather this frame's intent into a source-agnostic struct, then
            // apply it - the same struct the networked server applies remotely.
            in = PollLocalInput();
            if (!IsCursorHidden()) {
                in = PlayerInput{}; // cursor free (Esc, or window unfocused): no look/move/fire
            } else {
                // Swallow the cursor-centering jump + match-start click on the first
                // CAPTURED frame, not the first PLAYING frame (see networked path).
                if (consumeFirstLook) { in.lookDelta = {0, 0}; consumeFirstLook = false; }
                if (consumeFirstFire) { in.fire = false; consumeFirstFire = false; }
            }
            float gravity = in.earthGravity ? EARTH_GRAVITY : MOON_GRAVITY; // constants stay here
            ApplyPlayerInput(player, in, dt, gravity, gameSpace);

            // Drive every isBot slot through the behaviour tree (same input path
            // as the human above). Bots may hold the earth-gravity key to descend,
            // so drive() derives gravity per-bot from its input.
            botController.drive(gameSpace, dt);

            std::vector<Player>& players = gameSpace.getPlayers();
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
                audioQueue.push(fxTable[ev.fx], ev.pos, isLocal, ev.volumeScale);
            }
            gameSpace.getAudioEvents().clear();
        }

        // MARK: AUDIO QUEUE FLUSH
        // Play all queued sound events after the state updates. Skip while
        // networked-but-not-yet-connected: localPlayer is still null until the
        // first server packet (the connecting-screen guard below handles that
        // frame), and nothing has queued a sound yet anyway. This code only
        // runs on PLAYING frames (every other screen `continue`s before it);
        // stale-feed cleanup between matches lives in pumpNet /
        // returnToTitle / enterNetworkedMatch.
        if (localPlayer != nullptr)
            audioQueue.flush(*localPlayer);

        // MARK: MESSAGE CUE DRAIN
        // messages from the gameSpace are pushed into the local player queue.
        for (Message& msg : gameSpace.getMessages()) {
            messageQueue.push(msg);
        }
        // messages removed from gameSpace.
        gameSpace.getMessages().clear();


        // MARK: ESCAPE KEY / CURSOR TOGGLE
        if (IsKeyPressed(KEY_ESCAPE)) {
            // toggle cursor capture so you can alt-tab / quit comfortably. Re-arm
            // the first-look swallow on re-capture so the centering jump doesn't
            // leak into the aim (same reason it's armed at match start).
            if (IsCursorHidden()) EnableCursor(); else { DisableCursor(); consumeFirstLook = true; }
        }

        // MARK: DRAW
        // Networked mode before the server's welcome/first state arrives: there's
        // no local player yet, so show a connecting screen and skip the world draw.
        if (localPlayer == nullptr) {
            BeginDrawing();
                ClearBackground(BLACK);
                // "JOINING" only once the server has actually assigned us a slot
                // (myIndex); merely having an open socket isn't "in" yet - and for
                // UDP the socket is open instantly, before any welcome.
                const char* msg = myIndex >= 0 ? "JOINING GAME..." : "CONNECTING TO SERVER...";
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
        // 3. DRAW
        // Two passes: first render the 3D world into sceneTarget (capture),
        // then composite that texture to the screen - cleanly, or scrambled
        // by the damage glitch. No game state changes in here - purely visual.

        // Capture pass. Must be its own BeginTextureMode block, outside
        // BeginDrawing - you can't nest the two, and the frame has to exist
        // before it can be distorted. gameSpace.draw() is unchanged; it just
        // renders into the target now instead of the back buffer.
        // Far clip scales with the map so opposite-corner geometry never gets
        // culled (raylib's default 1000 truncates the arena past halfSize ~289).
        // rlSetClipPlanes just stores two doubles that BeginMode3D reads, so
        // per-frame is free and needs no plumbing where halfSize changes.
        rlSetClipPlanes(RL_CULL_DISTANCE_NEAR, fmax(1000.0, (double)gameSpace.getWalls().halfSize * 4.0));

        BeginTextureMode(sceneTarget);
            ClearBackground(BLACK);
            Camera3D sceneCam = CameraFromPlayer(player);
            BeginMode3D(sceneCam);
                // Stars first, centered on the camera (no parallax), depth-mask
                // off inside - everything after paints straight over them.
                DrawStarfield(sceneCam.position, (float)GetTime());
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
            
            // Glitch intensity ramps down to 0 at which point the player enters the spectating state.
            if (!player.isAlive && !player.isSpectating) {
                hurt = player.spectatingTimer / player.countdownToSpectating;
            }
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
                //     a chromatic-aberration fringe. Alpha scales with hurt too (not
                //     just the offset) so the tint fades out smoothly instead of
                //     staying full-strength until it snaps off at hurt == 0.
                float ca = 6.0f * hurt;
                unsigned char caA = (unsigned char)(160.0f * hurt);
                Rectangle fullSrc{0, 0, (float)tex.width, -(float)tex.height};
                BeginBlendMode(BLEND_ADDITIVE);
                    DrawTexturePro(tex, fullSrc, {-ca, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0.0f, {255, 0, 0, caA});
                    DrawTexturePro(tex, fullSrc, { ca, 0, (float)screenWidth, (float)screenHeight}, {0, 0}, 0.0f, {0, 80, 255, caA});
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
                DrawText(TextFormat("Rockets: %d", player.ammo), 10, textHeight * 1, 14, YELLOW);
                DrawText(TextFormat("Fuel: %.1f", player.fuel), 10, textHeight * 2, 14, GREEN);
                DrawText(TextFormat("Health: %d", player.health), 10, textHeight * 3, 14, RED);
                if (in.earthGravity) {
                    DrawText("EARTH GRAVITY ENGAGED!!!", 10, textHeight * 4, 14, BLUE);
                }
                DrawText(TextFormat("Score: %d", player.score), screenWidth - 80, textHeight * 1, 14, WHITE);
            }

            //MARK: Perf overlay (F3)
            // Gated only on the toggle (not isAlive) so it stays up while
            // spectating - benchmark runs outlive the pilot.
            if (perfOverlay) {
                float avg = 0.0f, mx = 0.0f;
                float sorted[120];
                for (int i = 0; i < perfCount; i++) {
                    sorted[i] = perfFrames[i];
                    avg += perfFrames[i];
                    mx = fmaxf(mx, perfFrames[i]);
                }
                avg /= (float)(perfCount > 0 ? perfCount : 1);
                int p95i = (perfCount * 95) / 100;
                std::nth_element(sorted, sorted + p95i, sorted + perfCount);
                float p95 = sorted[p95i];

                const char* l1 = TextFormat("FPS %.0f | avg %.2f ms | p95 %.2f | max %.2f",
                    1.0f / fmaxf(avg, 1e-6f), avg * 1000.0f, p95 * 1000.0f, mx * 1000.0f);
                const char* l2 = TextFormat("half %.0f | plat %d | roid %d | rock %d | spark %d | expl %d",
                    gameSpace.getWalls().halfSize,
                    (int)gameSpace.getPlatforms().size(), (int)gameSpace.getAsteroids().size(),
                    (int)gameSpace.getRockets().size(), (int)gameSpace.getSparks().size(),
                    (int)gameSpace.getExplosions().size());
                DrawText(l1, screenWidth - MeasureText(l1, 14) - 10, 10, 14, RAYWHITE);
                DrawText(l2, screenWidth - MeasureText(l2, 14) - 10, 28, 14, GRAY);

                if (GetTime() - perfLastLog > 5.0) {
                    perfLastLog = GetTime();
                    printf("PERF half=%.0f plat=%d roid=%d rock=%d spark=%d avg=%.2f p95=%.2f max=%.2f\n",
                        gameSpace.getWalls().halfSize,
                        (int)gameSpace.getPlatforms().size(), (int)gameSpace.getAsteroids().size(),
                        (int)gameSpace.getRockets().size(), (int)gameSpace.getSparks().size(),
                        avg * 1000.0f, p95 * 1000.0f, mx * 1000.0f);
                    fflush(stdout); // stdout is block-buffered when piped/redirected; each PERF row should land immediately
                }
            }

            // MARK: Message Queue Draw
            // Index-based loop (not range-for): remove() erases from the vector,
            // which invalidates a range-for's cached end() iterator.
            std::vector<Message>& messages = messageQueue.getMessages();
            for (int msg_index = 0; msg_index < (int)messages.size(); ) {
                Message& msg = messages[msg_index];

                std::string pa;
                std::string pb;
                // replace the local player's name with "YOU" for clarity in the
                // message queue. Match by id, not name - names can collide (two
                // un-named "PLAYER"s), ids are authoritative.
                if (msg.playerA_id == localPlayer->id) pa = "YOU"; else pa = msg.playerA_Name;
                if (msg.playerB_id == localPlayer->id) pb = "YOU"; else pb = msg.playerB_Name;
                if (pa == pb) pb = "YOURSELF"; // self-elimination (rocket hit own body) is possible
                msg.generate(pa, pb);
                bool visible = msg.visible(localPlayer->id);
                if (!visible) messageQueue.remove(msg_index); else msg_index++;
            }
            DrawMessageQueue(messageQueue, screenWidth, screenHeight);
            messageQueue.update(dt);


        EndDrawing();

        // The player just died: this frame already rendered with the death
        // glitch/greyscale; flip to GAME_OVER so the next frame shows the
        // end screen over the frozen world.
        // MARK: game over trigger

        
        if (networked) {
            // Manual end (M), networked: only the host - "player 1", the lowest
            // connected human slot, same rule as the START/OPTIONS gating - may
            // end the match, for everyone. This just REQUESTS the end; the
            // actual phase flip comes back from the server like any other match
            // end (isHostConn is the server-side backstop), so all clients run
            // the same game-over sequence below.
            if (IsCursorHidden() && IsKeyPressed(KEY_M) && net.isOpen()) {
                auto& ps = gameSpace.getPlayers();
                int hostSlot = -1;
                for (int i = 0; i < (int)ps.size(); i++)
                    if (ps[i].isConnected && !ps[i].isBot) { hostSlot = i; break; }
                if (myIndex >= 0 && myIndex == hostSlot)
                    net.send(serializeEndMatch());
            }

            // Networked: the SERVER decides when the match ends (last-player-
            // standing, or the host's end-match request above). Mirror the local
            // death-FX delay - count down before the
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
            // Manual end: M sends the match straight to the GAME_OVER screen
            // (no death-FX delay - this is a deliberate quit, not a death).
            // The networked equivalent (host-only request, branch above) goes
            // through the server instead. Gated on cursor capture like all
            // other game input. Chosen key is far from the WASD cluster so it
            // can't be fat-fingered mid-flight.
            if (IsCursorHidden() && IsKeyPressed(KEY_M)) {
                EnableCursor(); // free the cursor for the game-over menu
                screen = GameScreen::GAME_OVER;
            }

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
                // Last-man-standing: end once one (or zero) players remain, or no
                // humans are left. Asteroid count doesn't end a multiplayer match -
                // only a solo game (handled above) ends on clearing all asteroids.
                if (remaining_players <= 1 || remaining_humans <= 0) {
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

    // MARK: TEARDOWN
    // --- Teardown (runs once, after the loop exits) ---
    for (audioFX& fx : fxTable) fx.unload();
    for (MusicCue& mc : musicCueTable) mc.unload();
    CloseAudioDevice();
    UnloadRenderTexture(sceneTarget);
    UnloadShader(grayscaleShader);
    CloseWindow();
    return 0;
}
