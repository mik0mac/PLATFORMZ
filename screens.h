// screens.h
//
// The 2D game shell: which screen we are on, and the state each one owns.
//
// This is the menu-side counterpart to shapes.h (3D look) and ui.h (widgets).
// It exists because the entire shell used to live inside main() - GameScreen was
// declared as a local enum, and the title screen alone was ~390 lines of drawing
// wedged into the frame loop, reaching directly at forty-odd surrounding locals.
// Two more screens (BROWSE, LOBBY) cannot be added to that, so the state comes
// out first.
//
// Deliberately state + declarations only: the drawing stays in main.cpp for now.
// Moving 390 lines of layout in the same change as re-homing its state would make
// a diff nobody can review, and this half is mechanical and verifiable on its own.

#pragma once

#include "raylib.h"
#include "constants.h"
#include "options.h"
#include "random.h"
#include "wire.h"        // LeaderboardEntry, MatchSummary
#include "local_scores.h"// the LOCAL half of the HIGH SCORES modal
#include "ui.h"          // the immediate-mode widgets the screens are built from
#include "audio.h"       // MasterVolumeAmpToDb/DbToAmp (the volume slider)

#include <string>
#include <vector>

//MARK: Screens
// TITLE used to be three things at once: the menu, the local-match setup, and -
// in networked play - the live lobby of the one room that existed. With many
// rooms those are different places, and a player arriving could not tell which
// of the three they were looking at.
//
//   TITLE   a router, and nothing else. Asks one question: what kind of game?
//           No match options live here, and no gameplay starts from here.
//   LOCAL   offline setup: its own options, its own map size, START.
//   BROWSE  the rooms on a server. JOIN, QUICK MATCH, CREATE.
//   CUSTOM  name and visibility for a room you are about to host.
//   LOBBY   a room you are standing in: roster, its code, and either the host's
//           controls or what it is waiting for.
//
// LOCAL and CUSTOM deliberately do NOT share options. A local game and an online
// room are different things and are allowed to be configured differently.
enum class GameScreen { TITLE, LOCAL, BROWSE, CUSTOM, LOBBY, COUNTDOWN, PLAYING, GAME_OVER };

//MARK: Shell state
// Everything the menu screens own between frames. Not the game world, not the
// connection - those belong to main() and are handed in where a screen needs
// them.
struct ShellState {
    // ---- Name entry -----------------------------------------------------
    std::string playerName   = "PLAYER";
    bool        nameFocused  = true;  // field owns keyboard focus on entry (type without a click)
    bool        namePristine = true;  // still the untouched default; first keystroke clears it

    // ---- Modals ---------------------------------------------------------
    bool showControls = false;
    bool showOptions  = false;
    bool showScores   = false;   // leaderboard popup
    // Which board the popup is showing. There are two - the server's, shared by
    // everyone, and this machine's own LOCAL one - and they answer different
    // questions, so they are a tab apart rather than merged. Defaults to LOCAL
    // because that one always has something behind it; main() flips it to ONLINE
    // on open when there is a server, since that is the more interesting board
    // when it exists.
    bool scoresShowLocal = true;
    // This client's own best run, pinned under the board. The server omits it
    // when there is nothing to pin - no runs yet, or it is already up there - so
    // the client never has to decide.
    bool             hasPersonalBest = false;
    LeaderboardEntry personalBest;

    // ---- Slider drag latches --------------------------------------------
    // A slider being dragged must not be stomped by the server's echo of the
    // match options, which arrives in every state packet.
    bool sliderPlayersActive = false;
    bool sliderDiffActive    = false;
    bool sliderWElastActive  = false;
    bool sliderPElastActive  = false;
    bool sliderBoostActive   = false;
    bool sliderRSpeedActive  = false;
    bool sliderJThrustActive = false;
    bool sliderFBurnActive   = false;
    bool sliderFRegenActive  = false;
    bool sliderXRadiusActive = false;
    bool sliderVolumeActive  = false;

    // ---- Toggle echo guards ---------------------------------------------
    // Toggles have no drag latch, so instead we remember what we last sent and
    // accept a server value only when it differs - otherwise our own click gets
    // flipped back before its echo returns.
    bool optSentWalls = WALLS_ENABLED;
    bool optSentPhys  = ROCKETS_OBEY_PHYSICS;
    bool optSentFf    = FRIENDLY_FIRE;
    bool optSentCoast = COAST_MODE;

    // ---- Float shadows ---------------------------------------------------
    // UiSlider needs a float&; these three are ints in MatchOptions.
    float optNumPlayersF = (float)GAMESPACE_DEFAULT_PLAYERS;
    float optFuelBurnF   = (float)FUEL_CONSUMPTION_RATE;
    float optFuelRegenF  = (float)FUEL_REGEN_PCT_DEFAULT;

    // ---- Lobby / directory ----------------------------------------------
    std::vector<LeaderboardEntry> leaderboard;   // server-owned all-time table
    bool serverFull = false;                     // last join hit a full roster
    // Random, non-repeating order bot slots draw names in. Re-rolled on every
    // return to the title so each match gets a fresh set.
    std::vector<int> botNameOrder = ShuffledIndices(BOT_NAME_COUNT);

    // ---- Match browser (BROWSE) -----------------------------------------
    // EVERY room, not one page. The wire still pages - the reply is capped to one
    // datagram - but that is a transport fact the player should never meet, so
    // the client follows `next` to the end of the snapshot and concatenates. What
    // makes that safe is the snapshot itself: pages cut from a list re-sorted
    // between requests would assemble into one that never existed at any single
    // moment, with rooms appearing twice and others missing.
    std::vector<MatchSummary> matches;
    int   listCursor  = 0;                  // page we asked for
    int   listNext    = -1;                 // cursor for the next page, -1 = last
    int   listTotal   = 0;                  // rooms the server says exist
    double lastListAt = 0.0;                // GetTime() of the last refresh
    bool   awaitingList = false;            // a request is outstanding
    // Set while we are still walking the snapshot. Separate from awaitingList,
    // which is about a single request: this is "the list on screen is not the
    // whole list yet", and it is what the follow-up pages are driven from.
    int    listFollow    = -1;              // cursor still to fetch, -1 = done
    double listFollowAt  = 0.0;             // when we asked for it
    int    listFollowTry = 0;               // re-asks spent on it

    // ---- The background refresh -----------------------------------------
    // A room in GAMEOVER becomes a joinable lobby a minute later, and a room with
    // one seat left can lose it - so a list nobody refreshes starts lying, and
    // offers joins that bounce. But re-sorting the list under a player reaching
    // for a row is how they end up in the room NEXT to the one they aimed at,
    // which is why the old two-second poll had to go when the order became live.
    //
    // So the two halves of a refresh are separated. CONTENTS - how full a room
    // is, its phase, whether you can join it - are updated in place on a slow
    // timer, and nothing moves. ORDER and MEMBERSHIP only change when the player
    // presses REFRESH, which is the moment they are not mid-reach.
    std::vector<MatchSummary> listIncoming; // pages of a background refresh, staged
    bool   listMerging   = false;           // this walk is a background one
    double lastAutoListAt = 0.0;            // when the last background walk began
    int    listNewRooms  = 0;               // rooms the merge saw that we are not showing
    // Scroll offset in PIXELS, not rows. Smooth scrolling needs no more code than
    // row-stepping once the panel is clipped, and a part-row at the bottom edge
    // is the thing that tells a player there is more below.
    float  browseScrollPx = 0.0f;
    bool   browseDragging = false;          // the scrollbar thumb is held
    float  browseDragGrab = 0.0f;           // where in the thumb it was grabbed
    // The room we are actually in, straight from the welcome - not the code we
    // asked for. Quick match picks a room for us, and connecting with no room
    // named lands us in one we never chose, so only the server knows.
    std::string inMatchCode;
    MatchKind   inMatchKind = MatchKind::Custom;

    // Set when WE asked to move rooms (join / quick / create), so the welcome
    // that lands us somewhere new can be told apart from the one every client
    // gets on connect - and from the one that comes back after LEAVE, which
    // would otherwise bounce us straight into the room we just left.
    bool joinPending = false;
    // The move we asked for and have not been answered about, kept so it can be
    // asked AGAIN if the answer goes missing.
    //
    // Over UDP a welcome is just a datagram. Losing one leaves the server
    // believing we are seated while this client sits in the browser forever,
    // because nothing re-asks: the hello retry that used to cover it runs only
    // until the server first answers us (netAcked), and the silence-reset that
    // would notice runs only while we HOLD a slot. A dropped welcome falls
    // exactly between the two.
    //
    // Empty means "do not re-send this one" - a `create` would make a second
    // room, so it recovers by joining the code the `created` reply gave us.
    std::string pendingMoveMsg;
    double      pendingMoveAt = 0.0;
    int         pendingMoveTries = 0;
    bool roomChanged = false;   // a requested move completed; the screen acts on it
    // The mirror of roomChanged: we HELD a room and now hold none, because the
    // server said `unseated`. LEAVE is the ordinary way to get here and routes
    // itself, so this is for every other way - and it exists because a screen
    // that assumes a room (the lobby, the countdown) has to stop assuming one
    // the moment that stops being true, rather than sitting on a stale roster.
    bool roomLost = false;

    // ---- Custom match setup (CUSTOM) ------------------------------------
    std::string customName;                 // room name, defaulted from the player's
    bool        customNameFocused = false;
    bool        customPrivate = false;      // invite-only: hidden from the browser

    // The room THIS player created, from the server's `created` reply. Being host
    // is not the same as owning the room: walk into an empty room - the default
    // one every connection lands in, say - and you become its host by default.
    // Only a room you made is "your custom match" for the profile to remember.
    // Never needs clearing: room codes are never reused, so a stale value can
    // only ever fail to match.
    std::string createdCode;

    std::string joinCode;                   // JOIN CODE field contents
    bool        joinCodeFocused = false;
    // "COPIED" confirmation under the lobby's code, cleared on a timer - a copy
    // button with no feedback leaves you unsure whether it fired.
    std::string copyNotice;
    double      copyNoticeAt = 0.0;

    std::string browseStatus;               // one-line feedback, e.g. a refusal
    double      browseStatusAt = 0.0;       // when it was set, so it can fade

    void setBrowseStatus(const std::string& text, double now) {
        browseStatus   = text;
        browseStatusAt = now;
    }

    // Re-seed the per-match bits when coming back to the title screen.
    void onReturnToTitle() {
        botNameOrder = ShuffledIndices(BOT_NAME_COUNT);
        showControls = showOptions = showScores = false;
    }

    // Keep the float shadows in step after options change from the server.
    void syncShadows(const MatchOptions& opt) {
        optNumPlayersF = (float)opt.numPlayers;
        optFuelBurnF   = (float)opt.fuelConsumption;
        optFuelRegenF  = (float)opt.fuelRegenPct;
    }
};

//MARK: Shared modals
// The three popups a setup screen needs. They were inline in the title screen,
// which was fine while that was the only place they could be opened. The lobby
// and the local-match screen both need them now, and a second copy of a 130-line
// options panel is not a thing to maintain.
//
// Each is called only when its own flag is set; `wasOpen` is that flag SNAPSHOTTED
// at frame start, so the click that OPENS a modal cannot also hit the CLOSE button
// underneath it on the same frame.

inline void DrawControlsModal(ShellState& s, bool wasOpen) {
    Rectangle m = {250, 140, 500, 420};
    UiModalChrome(m, "CONTROLS");
    const char* lines[] = {
        "WASD          move",
        "Mouse         look",
        "Left click    fire rocket",
        "Space         jetpack (up)",
        "Left Shift    earth gravity enable",
        "M             end match (host only)",
        "Esc           toggle cursor capture",
        "+ / -         volume up/down",
    };
    int ly = (int)m.y + 60;
    for (const char* ln : lines) { DrawText(ln, (int)m.x + 40, ly, 18, RAYWHITE); ly += 34; }
    if (UiModalClose(m, wasOpen)) s.showControls = false;
}

// Two boards behind one modal, a tab apart.
//
// The arcade board (D5): the best RUNS, not career totals. One player can hold
// several rows - that is the point, and it is why beating your own third place is
// a normal evening rather than a milestone. Bots are on it too, sharing a single
// row between them: the factory high score, there to be knocked off.
//
// Below the board, this player's own best run. A top ten is invisible to everyone
// not in it, which is most people most of the time.
//
//MARK: ONLINE vs LOCAL
// ONLINE is read-only and server-owned: it pushes the table on join and after
// every credited match, so there is nothing to refresh from here, and IT decides
// whether to send the pin (absent when they have no runs, and absent when their
// best is already up there) - so the client never has to work that out.
//
// LOCAL is this machine's own, built here from local_scores.h, and fed only by
// offline matches. A local match is not refereed - the client hosts its own sim -
// so its scores can never join the shared table; see local_scores.h for the long
// version. Same rules, same shape, same modal: only the source differs, which is
// why both paths below collapse into one row renderer.
inline void DrawLeaderboardModal(ShellState& s, int screenWidth, bool wasOpen,
                                 bool onlineAvailable) {
    // 500, not the 420 every other modal uses: ten rows plus a pinned eleventh
    // plus the gap between them does not fit in 420, and silently clipping the
    // bottom of the board would be a strange way to find that out.
    Rectangle m = {250, 110, 500, 500};
    UiModalChrome(m, "HIGH SCORES");

    //MARK: Tabs
    // Offline, ONLINE is greyed rather than hidden - the same rule the title
    // screen's match buttons and the browser's unjoinable rows follow: a button
    // that vanishes reads as a bug, a greyed one reads as "not right now".
    const float tabW = 120.0f, tabH = 28.0f;
    const float tabY = m.y + 52.0f;
    auto tab = [&](float x, const char* label, bool selected, bool enabled) {
        Rectangle r = {x, tabY, tabW, tabH};
        if (selected) UiPanel(r, ui::OUTLINE, Fade(ui::FILL, 0.9f));
        int tw = MeasureText(label, 16);
        if (!enabled) {
            if (!selected) UiPanel(r, Fade(ui::OUTLINE, 0.3f), Fade(ui::FILL, 0.4f));
            DrawText(label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + 7), 16, GRAY);
            return false;
        }
        if (selected) {
            DrawText(label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + 7), 16, RAYWHITE);
            return false;   // already here; clicking it changes nothing
        }
        return wasOpen && UiButton(r, label, 16);
    };
    const float tabGap = 10.0f;
    const float tabLeft = m.x + m.width / 2.0f - tabW - tabGap / 2.0f;
    if (tab(tabLeft, "LOCAL", s.scoresShowLocal, true)) s.scoresShowLocal = true;
    if (tab(tabLeft + tabW + tabGap, "ONLINE", !s.scoresShowLocal, onlineAvailable))
        s.scoresShowLocal = false;
    // An open modal can outlive the connection that justified its tab (the socket
    // drops while you are reading). Fall back rather than showing a stale board
    // under a tab that is now greyed out.
    if (!onlineAvailable) s.scoresShowLocal = true;

    //MARK: The rows
    // Both sources reduced to the same two things - a list and an optional pin -
    // so the drawing below has no idea which board it is rendering.
    std::vector<LeaderboardEntry> rows;
    bool             hasPin = false;
    LeaderboardEntry pin;
    if (s.scoresShowLocal) {
        const localscores::Board& b = localscores::Get();
        for (const RunRow& r : b.top()) rows.push_back({r.name, r.score, r.isBot()});
        RunRow best;
        if (b.bestForPlayer(best) && !RunIsOnBoard(b.top(), best)) {
            hasPin = true;
            pin = {best.name, best.score, false};
        }
    } else {
        rows   = s.leaderboard;
        hasPin = s.hasPersonalBest;
        pin    = s.personalBest;
    }

    const int rowH = 30;
    int ly = (int)tabY + (int)tabH + 14;

    auto drawRow = [&](const char* label, const LeaderboardEntry& e, Color c) {
        const char* val = TextFormat("%d", e.score);
        DrawText(label, (int)m.x + 40, ly, 18, c);
        DrawText(val, (int)(m.x + m.width - 40 - MeasureText(val, 18)), ly, 18, ui::OUTLINE);
        ly += rowH;
    };

    if (rows.empty()) {
        // Distinguish "nothing recorded yet" from a broken panel - a fresh server
        // with no score file, and a first launch offline, both land here.
        UiTextCentered(s.scoresShowLocal ? "No local runs yet. Play a LOCAL MATCH."
                                         : "No runs recorded yet.",
                       screenWidth, (int)m.y + 150, 20, GRAY);
    } else {
        for (size_t i = 0; i < rows.size(); ++i) {
            drawRow(TextFormat("%d. %s", (int)i + 1, rows[i].name.c_str()), rows[i],
                    rows[i].isBot ? ui::OUTLINE : RAYWHITE);
        }
    }

    if (hasPin) {
        // A gap and a rule, so the pin reads as "and yours" rather than as an
        // eleventh place.
        ly += 10;
        DrawLine((int)m.x + 40, ly, (int)(m.x + m.width - 40), ly, ui::OUTLINE);
        ly += 12;
        drawRow(TextFormat("YOUR BEST  %s", pin.name.c_str()), pin, RAYWHITE);
    }

    if (UiModalClose(m, wasOpen)) s.showScores = false;
}

// Two columns of five sliders plus a row of four gameplay toggles. Returns true
// on any frame a control changed, so a networked caller can push the new bundle
// to the server; a local caller ignores it. The modal itself knows nothing about
// the connection - that stayed in main().
inline bool DrawOptionsModal(ShellState& s, MatchOptions& opt, bool wasOpen) {
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
    valueAt(TextFormat("%d", (int)s.optNumPlayersF), lxL, y1);
    if (UiSlider({lxL, (float)(y1 + 26), colW, 22}, s.optNumPlayersF,
             OPT_RANGE_NUM_PLAYERS.min, OPT_RANGE_NUM_PLAYERS.max,
             s.sliderPlayersActive, OPT_RANGE_NUM_PLAYERS.step)) {
        opt.numPlayers = (int)s.optNumPlayersF; optChanged = true;
    }

    // BOT DIFFICULTY (continuous, 0.0..BOT_DIFFICULTY).
    DrawText("BOT DIFFICULTY", (int)lxL, y2, 18, RAYWHITE);
    valueAt(TextFormat("%.2f", opt.botDifficulty), lxL, y2);
    if (UiSlider({lxL, (float)(y2 + 26), colW, 22}, opt.botDifficulty,
             OPT_RANGE_BOT_DIFFICULTY.min, OPT_RANGE_BOT_DIFFICULTY.max,
             s.sliderDiffActive, OPT_RANGE_BOT_DIFFICULTY.step)) optChanged = true;

    // WALL ELASTICITY (players only; asteroids keep their constant).
    DrawText("WALL ELASTICITY", (int)lxL, y3, 18, RAYWHITE);
    valueAt(TextFormat("%.2f", opt.wallElasticity), lxL, y3);
    if (UiSlider({lxL, (float)(y3 + 26), colW, 22}, opt.wallElasticity,
             OPT_RANGE_WALL_ELASTICITY.min, OPT_RANGE_WALL_ELASTICITY.max,
             s.sliderWElastActive, OPT_RANGE_WALL_ELASTICITY.step)) optChanged = true;

    // PLATFORM ELASTICITY (players only; asteroids keep their constant).
    DrawText("PLATFORM ELASTICITY", (int)lxL, y4, 18, RAYWHITE);
    valueAt(TextFormat("%.2f", opt.platformElasticity), lxL, y4);
    if (UiSlider({lxL, (float)(y4 + 26), colW, 22}, opt.platformElasticity,
             OPT_RANGE_PLATFORM_ELASTICITY.min, OPT_RANGE_PLATFORM_ELASTICITY.max,
             s.sliderPElastActive, OPT_RANGE_PLATFORM_ELASTICITY.step)) optChanged = true;

    // SPEED BOOST (walk + jetpack speed/accel, and rocket speed).
    DrawText("SPEED BOOST", (int)lxL, y5, 18, RAYWHITE);
    valueAt(TextFormat("%.1fx", opt.speedBoost), lxL, y5);
    if (UiSlider({lxL, (float)(y5 + 26), colW, 22}, opt.speedBoost,
             OPT_RANGE_SPEED_BOOST.min, OPT_RANGE_SPEED_BOOST.max,
             s.sliderBoostActive, OPT_RANGE_SPEED_BOOST.step)) optChanged = true;

    // --- Right column ---
    // ROCKET VELOCITY (on top of SPEED BOOST).
    DrawText("ROCKET VELOCITY", (int)lxR, y1, 18, RAYWHITE);
    valueAt(TextFormat("%.1fx", opt.rocketSpeedScale), lxR, y1);
    if (UiSlider({lxR, (float)(y1 + 26), colW, 22}, opt.rocketSpeedScale,
             OPT_RANGE_ROCKET_SPEED.min, OPT_RANGE_ROCKET_SPEED.max,
             s.sliderRSpeedActive, OPT_RANGE_ROCKET_SPEED.step)) optChanged = true;

    // JETPACK THRUST (on top of SPEED BOOST; jetpack only).
    DrawText("JETPACK THRUST", (int)lxR, y2, 18, RAYWHITE);
    valueAt(TextFormat("%.1fx", opt.jetpackThrust), lxR, y2);
    if (UiSlider({lxR, (float)(y2 + 26), colW, 22}, opt.jetpackThrust,
             OPT_RANGE_JETPACK_THRUST.min, OPT_RANGE_JETPACK_THRUST.max,
             s.sliderJThrustActive, OPT_RANGE_JETPACK_THRUST.step)) optChanged = true;

    // FUEL CONSUMPTION (direct units/sec out of the 100-unit tank).
    DrawText("FUEL CONSUMPTION (%)", (int)lxR, y3, 18, RAYWHITE);
    valueAt(TextFormat("%d/sec", (int)s.optFuelBurnF), lxR, y3);
    if (UiSlider({lxR, (float)(y3 + 26), colW, 22}, s.optFuelBurnF,
             OPT_RANGE_FUEL_CONSUMPTION.min, OPT_RANGE_FUEL_CONSUMPTION.max,
             s.sliderFBurnActive, OPT_RANGE_FUEL_CONSUMPTION.step)) {
        opt.fuelConsumption = (int)s.optFuelBurnF; optChanged = true;
    }

    // FUEL REGEN (percentage of the consumption rate; 100% = keeps pace).
    DrawText("FUEL REGEN (% of consmpt.)", (int)lxR, y4, 18, RAYWHITE);
    valueAt(TextFormat("%d/sec", (int)s.optFuelRegenF), lxR, y4);
    if (UiSlider({lxR, (float)(y4 + 26), colW, 22}, s.optFuelRegenF,
             OPT_RANGE_FUEL_REGEN.min, OPT_RANGE_FUEL_REGEN.max,
             s.sliderFRegenActive, OPT_RANGE_FUEL_REGEN.step)) {
        opt.fuelRegenPct = (int)s.optFuelRegenF; optChanged = true;
    }

    // EXPLOSION RADIUS (damage radius + blast visual; last in the
    // modal per its own domain, grouped away from the speed trio).
    DrawText("EXPLOSION RADIUS", (int)lxR, y5, 18, RAYWHITE);
    valueAt(TextFormat("%.1fx", opt.explosionRadiusScale), lxR, y5);
    if (UiSlider({lxR, (float)(y5 + 26), colW, 22}, opt.explosionRadiusScale,
             OPT_RANGE_EXPLOSION_RADIUS.min, OPT_RANGE_EXPLOSION_RADIUS.max,
             s.sliderXRadiusActive, OPT_RANGE_EXPLOSION_RADIUS.step)) optChanged = true;

    // Toggles: three across, label on its own line, a compact ON/OFF
    // control below (labels are long, so keep them off the control's
    // line). Each defaults to its options.h value; applied at match
    // start. Sliders use an 85px rhythm; this row sits just below them.
    // Four explicit x positions (not the slider columns), spaced by
    // measured label width at font 18 - 157 / 134 / 115 / 220 px
    // left to right - so no label runs into its neighbor. The
    // widest, ROCKETS OBEY PHYSICS, goes last, where it has the
    // panel's right edge (m.x + m.width = 890) to grow into: it
    // ends at 870. Each toggle sits under its label's left edge.
    int y6 = y5 + 85;
    float txBoundary = lxL;          // 150
    float txFriendly = lxL + 190.0f; // 340
    float txCoast    = lxL + 350.0f; // 500
    float txPhysics  = lxL + 500.0f; // 650

    DrawText("BOUNDARY WALLS", (int)txBoundary, y6, 18, RAYWHITE);
    if (UiToggle({txBoundary, (float)(y6 + 26), 100, 24}, opt.wallsEnabled)) {
        optChanged = true; s.optSentWalls = opt.wallsEnabled;
    }

    DrawText("FRIENDLY FIRE", (int)txFriendly, y6, 18, RAYWHITE);
    if (UiToggle({txFriendly, (float)(y6 + 26), 100, 24}, opt.friendlyFire)) {
        optChanged = true; s.optSentFf = opt.friendlyFire;
    }

    DrawText("COAST MODE", (int)txCoast, y6, 18, RAYWHITE);
    if (UiToggle({txCoast, (float)(y6 + 26), 100, 24}, opt.coastMode)) {
        optChanged = true; s.optSentCoast = opt.coastMode;
    }

    DrawText("ROCKETS OBEY PHYSICS", (int)txPhysics, y6, 18, RAYWHITE);
    if (UiToggle({txPhysics, (float)(y6 + 26), 100, 24}, opt.rocketsObeyPhysics)) {
        optChanged = true; s.optSentPhys = opt.rocketsObeyPhysics;
    }

    if (UiModalClose(m, wasOpen)) s.showOptions = false;
    return optChanged;
}

//MARK: Refusals
// The server's answer to something the player asked for, on whatever screen they
// asked from.
//
// This used to be drawn inline in DrawBrowse and NOWHERE ELSE, which made a
// refusal invisible to anyone who was not standing in the browser. CREATE is on
// the CUSTOM screen, so an over-budget create - "you already have three rooms" -
// set this string and then rendered on a screen the player had left behind. From
// where they stood the button simply did nothing, which is the worst possible
// report of a refusal: no error, no log line, nothing to search for.
//
// Fades after a few seconds so a stale message is never mistaken for the current
// state.
inline void DrawRefusalLine(const ShellState& s, int screenW, int y, double now) {
    if (s.browseStatus.empty()) return;
    const double age = now - s.browseStatusAt;
    if (age >= 6.0) return;
    Color c = age > 4.0 ? Fade(RED, (float)((6.0 - age) / 2.0)) : RED;
    UiTextCentered(s.browseStatus.c_str(), screenW, y, 18, c);
}

//MARK: BROWSE
// What the player asked for this frame. The screen reports intent only - main()
// owns the socket and decides what to send - so this stays free of networking.
// No Page action any more: the list is scrolled, not paged. Paging survives on
// the WIRE (the reply is capped to one datagram) but the client walks it to the
// end and hands the player one list, so there is no page for them to ask for.
enum class BrowseAction { None, Back, Refresh, Quick, Create, Join };

struct BrowseResult {
    BrowseAction action = BrowseAction::None;
    std::string  code;       // Join: which room, from a row or the code field
    std::string  joinCode;   // Join: a private room's password, if one was typed
};

// The match browser. Immediate mode like everything in ui.h: call it every frame,
// it draws and returns this frame's interaction.
//
// `connected` gates the actions rather than hiding them. Buttons that vanish when
// the socket drops read as a rendering bug; buttons that are visibly inert read
// as "not right now".
inline BrowseResult DrawBrowse(ShellState& s, int screenW, int screenH,
                               bool connected, double now) {
    (void)screenH;
    BrowseResult out;

    const float listX = 100.0f, listY = 150.0f;
    const float listW = (float)screenW - 200.0f, listH = 330.0f;
    const float rowH  = 40.0f;
    const Rectangle listRect = {listX, listY, listW, listH};

    UiTextCentered("FIND A MATCH", screenW, 80, 40, RAYWHITE);

    // REFRESH is the ONLY thing that re-orders this list (the browser stopped
    // polling when the order became live), so it carries more weight than it used
    // to - and it is worth not letting a player spend it on nothing. The server
    // allows a small burst and then one list a second, and drops anything over
    // budget WITHOUT A REPLY, so a mashed button would sit on "LOOKING FOR
    // MATCHES..." with no request left alive to answer it. Inert for a second
    // after each ask, drawn the same way an unjoinable row's button is: visible
    // and obviously not available, rather than missing.
    const Rectangle refreshBtn = {listX + listW - 130.0f, listY - 46.0f, 130.0f, 34.0f};
    const bool refreshReady = connected && (now - s.lastListAt) >= 1.0;
    if (refreshReady) {
        if (UiButton(refreshBtn, "REFRESH", 18)) out.action = BrowseAction::Refresh;
    } else {
        UiPanel(refreshBtn, Fade(ui::OUTLINE, 0.3f), Fade(ui::FILL, 0.4f));
        int tw = MeasureText("REFRESH", 18);
        DrawText("REFRESH", (int)(refreshBtn.x + (refreshBtn.width - tw) / 2),
                 (int)(refreshBtn.y + 8), 18, GRAY);
    }

    // Total count, so a capped page is not mistaken for the whole world. While
    // the follow-up pages are still arriving this and matches.size() disagree,
    // which is exactly right: it says how many there ARE while the list fills in.
    //
    // ROOMS, not MATCHES. A room outlives the match inside it - the row you are
    // counting may be a lobby, a game in progress, or one winding down - so
    // counting "matches" promised a game where there was only a place to wait.
    // The screen is still called FIND A MATCH, because a match is what a player
    // came for; what the list holds is the rooms they are held in.
    DrawText(s.listTotal == 1 ? "1 ROOM" : TextFormat("%d ROOMS", s.listTotal),
             (int)listX, (int)listY - 40, 18, ui::OUTLINE);
    // Rooms a background refresh found that are not in the rows. They are not
    // inserted, because inserting shifts every row below the insertion point and
    // the whole reason that refresh merges is to never move anything. So they are
    // OFFERED instead, and REFRESH is what takes them.
    if (s.listNewRooms > 0) {
        const char* n = s.listNewRooms == 1 ? "1 NEW - REFRESH"
                                            : TextFormat("%d NEW - REFRESH", s.listNewRooms);
        DrawText(n, (int)listX + 130, (int)listY - 40, 18, ui::OUTLINE);
    }

    UiPanel(listRect);

    // SCROLLING. The content is every room the snapshot held, so it can be taller
    // than the panel; `browseScrollPx` is how far down it has been pushed.
    const float contentH = (float)s.matches.size() * rowH + 12.0f;
    const float maxScroll = contentH > listH ? contentH - listH : 0.0f;
    const bool  mouseOverList = CheckCollisionPointRec(GetMousePosition(), listRect);

    // The wheel only steers while the pointer is over the list, so a scroll aimed
    // at the page does not quietly move a row out from under the cursor.
    if (mouseOverList) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) s.browseScrollPx -= wheel * rowH;
    }
    // Arrow keys do the same, for a trackpad-less mouse and for anyone who never
    // thinks to scroll a list that has no visible bar until it overflows.
    if (IsKeyDown(KEY_DOWN)) s.browseScrollPx += rowH * 0.25f;
    if (IsKeyDown(KEY_UP))   s.browseScrollPx -= rowH * 0.25f;

    // The bar. Drawn only when there is something to scroll - a permanent bar on
    // a list of five rooms is furniture that means nothing.
    const Rectangle barTrack = {listX + listW - 9.0f, listY + 4.0f, 6.0f, listH - 8.0f};
    if (maxScroll > 0.0f) {
        const float thumbH = std::max(24.0f, barTrack.height * (listH / contentH));
        const float travel = barTrack.height - thumbH;
        const Rectangle thumb = {barTrack.x, barTrack.y + travel * (s.browseScrollPx / maxScroll),
                                 barTrack.width, thumbH};
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(), thumb)) {
            s.browseDragging = true;
            s.browseDragGrab = GetMousePosition().y - thumb.y;
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) s.browseDragging = false;
        if (s.browseDragging && travel > 0.0f)
            s.browseScrollPx = ((GetMousePosition().y - s.browseDragGrab) - barTrack.y)
                             / travel * maxScroll;

        DrawRectangleRec(barTrack, Fade(ui::OUTLINE, 0.15f));
        DrawRectangleRec(thumb, s.browseDragging ? ui::OUTLINE : Fade(ui::OUTLINE, 0.55f));
    } else {
        s.browseDragging = false;
    }
    // Clamp AFTER every input, and after the list has changed size under us - a
    // refresh that returns fewer rooms must not leave us scrolled past the end.
    if (s.browseScrollPx > maxScroll) s.browseScrollPx = maxScroll;
    if (s.browseScrollPx < 0.0f)      s.browseScrollPx = 0.0f;

    if (!connected) {
        UiTextCentered("NOT CONNECTED", screenW, (int)(listY + listH / 2 - 10), 20, GRAY);
    } else if (s.matches.empty()) {
        // "Empty" and "still loading" look identical unless you say which it is.
        UiTextCentered(s.awaitingList ? "LOOKING FOR MATCHES..." : "NO MATCHES YET",
                       screenW, (int)(listY + listH / 2 - 20), 20, ui::OUTLINE);
        if (!s.awaitingList)
            UiTextCentered("CREATE ONE, OR TRY QUICK MATCH",
                           screenW, (int)(listY + listH / 2 + 8), 16, RAYWHITE);
    } else {
        // Clip to the panel so a row scrolled half past the edge is cut rather
        // than drawn over the frame - raylib's scissor is glScissor, so this
        // works the same in the browser. The part-row it leaves at the bottom is
        // the affordance: it is what says "there is more below".
        BeginScissorMode((int)listRect.x, (int)listRect.y,
                         (int)listRect.width, (int)listRect.height);
        for (int idx = 0; idx < (int)s.matches.size(); ++idx) {
            const MatchSummary& m = s.matches[idx];
            const float ry = listY + 6.0f + idx * rowH - s.browseScrollPx;
            // Nothing to draw and nothing to click, well off either edge.
            if (ry + rowH < listRect.y || ry > listRect.y + listRect.height) continue;

            // A room a background refresh could no longer find: reaped, or gone
            // private. Drawn DEAD IN PLACE rather than removed - taking the row
            // out would shift every row below it, and not moving rows under the
            // player is the whole point of merging instead of replacing. REFRESH
            // is what clears it away, because that is when re-ordering is asked
            // for.
            const bool dead = m.gone;
            // GREY MEANS ONE THING: you cannot have this. A row you can walk
            // into is drawn in full colour, every field of it; a row you cannot -
            // reaped, full, or winding down - goes grey WHOLE, the same grey the
            // dead REFRESH button and the disabled title-screen rows use. It used
            // to be spent on ordinary information instead (the map, a CUSTOM tag,
            // any phase but "playing"), which left a joinable room looking half
            // disabled and a full one looking joinable.
            const bool live = !dead && m.joinable;
            // Two live colours under that rule. The ACCENT classifies the room -
            // how full it is, and who runs it - and white carries the rest.
            const Color accent    = live ? ui::OUTLINE : GRAY;
            const Color secondary = live ? RAYWHITE    : GRAY;
            DrawText(m.name.c_str(), (int)listX + 14, (int)ry + 10, 18, secondary);
            DrawText(dead ? "--" : TextFormat("%d/%d", m.players, m.maxPlayers),
                     (int)listX + 300, (int)ry + 10, 18, accent);
            // The arena, which the browser could not show at all until the map
            // moved into MatchOptions - before that it did not exist until
            // somebody pressed a START button.
            DrawText(m.map.c_str(), (int)listX + 360, (int)ry + 10, 16, secondary);
            // Kind, not preset. "DEFAULT" in every row tells a player nothing,
            // where OFFICIAL vs CUSTOM tells them whether the rules are fixed and
            // the room starts itself, or whether somebody is running it and
            // decides both. SAME colour for both: they are two answers to one
            // question, and drawing CUSTOM in grey said "this row is worth less"
            // when it only ever meant "somebody is running this one".
            const bool official = (m.kind == MatchKind::Official);
            DrawText(official ? "OFFICIAL" : "CUSTOM",
                     (int)listX + 460, (int)ry + 10, 16, accent);
            DrawText(dead ? "closed" : m.phase.c_str(), (int)listX + 570, (int)ry + 10, 16,
                     secondary);

            // Clear of the scrollbar at listW-9: this ends at listW-14.
            Rectangle joinBtn = {listX + listW - 100.0f, ry + 4.0f, 86.0f, 30.0f};
            if (m.joinable && !dead) {
                // `mouseOverList` is what keeps a half-scrolled row honest. The
                // scissor clips what is DRAWN, not what UiButton hit-tests, so
                // without it the invisible half of a button scrolled past the
                // panel edge would still take a click - from a spot where the
                // player can see a different row entirely.
                if (UiButton(joinBtn, "JOIN", 16) && connected && mouseOverList) {
                    out.action = BrowseAction::Join;
                    out.code   = m.code;
                }
            } else {
                // Inert but drawn: a row that just loses its button looks broken,
                // where a greyed reason explains itself.
                //
                // SAY WHICH REASON. A room is unjoinable either because it has no
                // seat or because it is winding down, and labelling both "FULL"
                // meant a 1/4 room that had just ended read as full - for the 60 s
                // of GAMEOVER_LOBBY_SECONDS, after every single match. The row
                // already carries the phase and the counts, so no server help is
                // needed to tell the two apart.
                const char* why = dead ? "GONE"
                                : (m.phase == "gameover") ? "ENDING"
                                : (m.players >= m.maxPlayers) ? "FULL"
                                : "CLOSED";   // shouldn't happen; better than lying
                UiPanel(joinBtn, Fade(ui::OUTLINE, 0.3f), Fade(ui::FILL, 0.4f));
                int tw = MeasureText(why, 16);
                DrawText(why, (int)(joinBtn.x + (joinBtn.width - tw) / 2),
                         (int)(joinBtn.y + 7), 16, GRAY);
            }
        }
        EndScissorMode();
    }

    // Still filling in. Said under the panel rather than over the rows, because
    // the rows that HAVE arrived are already usable - this is "more on the way",
    // not "wait".
    if (s.listFollow >= 0 && connected)
        DrawText(TextFormat("LOADING %d OF %d...", (int)s.matches.size(), s.listTotal),
                 (int)listX, (int)(listY + listH + 10), 16, RAYWHITE);

    const float by = listY + listH + 56.0f;
    if (UiButton({listX, by, 170.0f, 44.0f}, "QUICK MATCH") && connected)
        out.action = BrowseAction::Quick;
    // CUSTOM MATCH, matching the title screen's word for the same destination -
    // it is the same CUSTOM screen either way, and a player who took CUSTOM MATCH
    // from the router should not have to work out that CREATE MATCH is where it
    // went. 190 wide rather than 170: the longer label had 15px of air either
    // side of it, which reads as a label that only just fit.
    if (UiButton({listX + 190.0f, by, 190.0f, 44.0f}, "CUSTOM MATCH") && connected)
        out.action = BrowseAction::Create;

    // Join by code, for a private room whose code arrived out of band. Shifted
    // right with the wider button above, and GO trimmed to 90, so the row still
    // clears BACK at the far edge.
    DrawText("CODE", (int)listX + 400, (int)by + 14, 16, ui::OUTLINE);
    UiTextField({listX + 450.0f, by, 110.0f, 44.0f}, s.joinCode, s.joinCodeFocused, 8, 20);
    if (UiButton({listX + 580.0f, by, 90.0f, 44.0f}, "GO", 20) && connected && !s.joinCode.empty()) {
        out.action   = BrowseAction::Join;
        out.code     = s.joinCode;
        out.joinCode = s.joinCode;  // a private room's code doubles as its password
    }

    if (UiButton({listX + listW - 110.0f, by, 110.0f, 44.0f}, "BACK"))
        out.action = BrowseAction::Back;

    DrawRefusalLine(s, screenW, (int)by + 60, now);
    return out;
}

//MARK: Who is the host
// Whatever slot the SERVER flagged - never recomputed from slot order. The host
// is the room's CREATOR, not the lowest slot, and an official room has none at
// all, so "lowest connected human" is the wrong answer in both directions.
// Returns -1 for a hostless room.
//
// Lives here rather than inside DrawLobby because main.cpp needs the same answer
// to decide whether the current room's options are this player's to remember.
inline int HostSlot(const std::vector<Player>& players) {
    for (int i = 0; i < (int)players.size(); ++i)
        if (players[i].isHost) return i;
    return -1;
}

//MARK: Shared chrome
// Master volume, pinned bottom-right on every setup screen. Rides the dB scale
// (0 dB full, MASTER_VOLUME_MIN_DB = mute at the far left) so track travel
// matches perceived loudness instead of bunching everything audible into the top
// of the range. Reads and writes raylib's master volume directly - no shadow
// copy - so it and the +/- keys can never disagree about the level.
inline void DrawVolumeSlider(ShellState& s, int screenWidth, int screenHeight, bool uiEnabled) {
    const float volW = 200.0f;
    Rectangle volTrack = {screenWidth - volW - 30.0f, screenHeight - 44.0f, volW, 22.0f};
    float volDb = MasterVolumeAmpToDb(GetMasterVolume());
    DrawText("VOLUME", (int)volTrack.x, (int)volTrack.y - 26, 18, RAYWHITE);
    const char* volVal = volDb <= MASTER_VOLUME_MIN_DB ? "MUTE"
                       : TextFormat("%d dB", (int)roundf(volDb));
    DrawText(volVal, (int)(volTrack.x + volW - MeasureText(volVal, 18)),
             (int)volTrack.y - 26, 18, ui::OUTLINE);
    // No feedback blip here (unlike the +/- keys): the slider shows the level on
    // screen, and a drag would machine-gun the sound.
    if (uiEnabled && UiSlider(volTrack, volDb, MASTER_VOLUME_MIN_DB, 0.0f, s.sliderVolumeActive))
        SetMasterVolume(MasterVolumeDbToAmp(volDb));
}

// The roster panel, shared by LOBBY (the live room) and LOCAL (a preview of the
// roster the offline match will build). Returns the panel's bottom edge so the
// caller can stack under a box whose height depends on the row count.
//
// `players` is the client's mirror of the server's slots in networked play, and
// the local sim's own slots offline. `myName` is passed in rather than read off
// ShellState because it is the LIVE-TYPED name, defaults included, which only
// main() can resolve.
inline float DrawRosterPanel(ShellState& s, const std::vector<Player>& players,
                             int myIndex, const std::string& myName,
                             const MatchOptions& opt, bool networked,
                             const std::string& roomCode, MatchKind roomKind,
                             float top) {
    int rowsShown;
    int previewCount = 0;   // networked: rows to draw (slots 0..previewCount-1)
    if (networked) {
        // Never hide a connected human sitting above the chosen count - a
        // mid-roster slot can be free while a higher one is taken.
        int lastHumanSlot = -1;
        for (int i = 0; i < (int)players.size(); ++i)
            if (players[i].isConnected && !players[i].isBot) lastHumanSlot = i;
        previewCount = std::min(std::max(opt.numPlayers, lastHumanSlot + 1), (int)players.size());
        rowsShown = previewCount > 0 ? previewCount : 1;   // >=1 so the "waiting" line has a row
    } else {
        rowsShown = opt.numPlayers;   // the OPTIONS slider previews the roster
    }

    const float rowH = 24.0f, headerH = 30.0f;
    // The panel grows with the roster - eight slots is 232px - and everything
    // below it is stacked from its bottom edge. `top` is what keeps a full house
    // from pushing the buttons off the window, which a fixed 300 did.
    Rectangle box = {350, top, 300, headerH + rowsShown * rowH + 10.0f};
    UiPanel(box);
    DrawText("PLAYERS", (int)box.x + 10, (int)box.y + 8, 14, ui::OUTLINE);

    // Which room this is, right-aligned in the header. The code IS the invite for
    // an invite-only room, so a player who cannot see it cannot ask anyone to join
    // them.
    if (networked && !roomCode.empty()) {
        const char* tag   = roomKind == MatchKind::Official ? "OFFICIAL" : "ROOM";
        const char* label = TextFormat("%s %s", tag, roomCode.c_str());
        DrawText(label, (int)(box.x + box.width - 10 - MeasureText(label, 14)),
                 (int)box.y + 8, 14, GRAY);
    }

    if (networked) {
        if (previewCount == 0)
            DrawText("Waiting for players...", (int)box.x + 10, (int)(box.y + headerH), 18, GRAY);
        // Slots 0..previewCount-1 are contiguous rows, but they are no longer all
        // occupied: a room whose preset caps maxBots leaves the slots past the cap
        // genuinely empty. Those arrive neither connected nor bot-driven, and are
        // drawn as OPEN - they are real, joinable seats, not missing rows.
        for (int i = 0; i < previewCount; ++i) {
            int  ry  = (int)(box.y + headerH + i * rowH);
            bool you = (i == myIndex);
            if (!you && !players[i].isConnected && !players[i].isBot) {
                DrawText(TextFormat("%d. -- OPEN --", i + 1), (int)box.x + 10, ry, 18, GRAY);
                continue;
            }
            // Our row shows the live-typed name; other rows show the server-synced
            // name, falling back to a slot label until they have set one.
            std::string shown = you ? myName
                : (players[i].name.empty() ? TextFormat("PLAYER %d", i + 1) : players[i].name);
            DrawText(TextFormat("%d. %s%s", i + 1, shown.c_str(), you ? " (YOU)" : ""),
                     (int)box.x + 10, ry, 18, you ? RAYWHITE : ui::OUTLINE);
        }
    } else {
        for (int i = 0; i < rowsShown; ++i) {
            int ry = (int)(box.y + headerH + i * rowH);
            if (i == 0)
                DrawText(TextFormat("1. %s (YOU)", myName.c_str()), (int)box.x + 10, ry, 18, RAYWHITE);
            else
                DrawText(TextFormat("%d. %s", i + 1,
                                    BOT_NAME_STRINGS[s.botNameOrder[(i - 1) % BOT_NAME_COUNT]]),
                         (int)box.x + 10, ry, 18, ui::OUTLINE);
        }
    }
    return box.y + box.height;
}

// The map SELECTOR plus one START, shared by LOCAL and by a custom room's host.
//
// These used to be four START buttons - the map was whichever one you pressed,
// so it existed only at the instant of starting and nobody else in the lobby
// could see it. Selecting and starting are separate acts now: the choice goes
// into MatchOptions, rides the options bundle to everyone, and START is START.
//
// Writes the picked map into `opt` and returns true when START was pressed.
inline bool DrawMapSelector(MatchOptions& opt, float y, bool uiEnabled, bool canStart) {
    DrawText("MAP", 110, (int)y - 22, 16, ui::OUTLINE);
    for (int i = 0; i < MAP_SIZE_COUNT; ++i) {
        Rectangle r = {110.0f + i * 145.0f, y, 130.0f, 42.0f};
        const bool chosen = (opt.mapSize == mapSizeOrder[i]);
        // The current pick reads as pressed rather than merely available - with
        // four look-alike buttons and no START among them, nothing else says
        // which arena you are about to play.
        if (chosen) UiPanel(r, ui::OUTLINE, ui::FILL_HI);
        if (uiEnabled && UiButton(r, mapSizeOrder[i], 18) && !chosen)
            opt.mapSize = mapSizeOrder[i];
    }
    if (!canStart) return false;
    return uiEnabled && UiButton({700.0f, y, 190.0f, 42.0f}, "START", 20);
}

//MARK: TITLE
// A router, and nothing else. Every path below leads somewhere that configures
// and starts a game; this screen only asks which one.
//
// No OPTIONS button, on purpose. Options configure the match you are about to
// start, which makes no sense on a screen that starts nothing - and the three
// destinations do not share a rule set anyway.
enum class TitleAction { None, QuickMatch, FindMatch, CustomMatch, LocalMatch,
                         Controls, Leaderboard, Quit };

inline TitleAction DrawTitle(ShellState& s, int screenWidth, int screenHeight,
                             bool networked, bool connected, bool uiEnabled,
                             bool& nameEdited) {
    TitleAction action = TitleAction::None;

    UiTextCentered("PLATFORMZ", screenWidth, 110, 80, RAYWHITE);

    // Name entry stays: it is identity for every path below it.
    UiTextCentered("NAME", screenWidth, 215, 20, ui::OUTLINE);
    Rectangle nameBox = {350, 240, 300, 40};
    // Let the name fill the space the UI allots but never overflow it. The
    // tightest renderer is the lobby roster row: "%d. NAME (YOU)" at font 18
    // inside the 300px players panel. Convert its leftover width to the field's
    // font size (20) and let UiTextField reject chars past that budget.
    int nameBudget = (280 - MeasureText("8. ", 18) - MeasureText(" (YOU)", 18)) * 20 / 18;
    nameEdited = UiTextField(nameBox, s.playerName, s.nameFocused,
                             PLAYER_NAME_MAX_CHARS, 20, &s.namePristine, nameBudget);

    float y = 322.0f;
    auto row = [&](const char* label, bool enabled) {
        Rectangle r = {350, y, 300, 48};
        y += 56.0f;
        if (!enabled) {
            // DISABLED, not hidden. A button that vanishes reads as a bug; a
            // greyed one reads as "not right now" - the same rule the browser's
            // unjoinable rows follow.
            UiPanel(r, Fade(ui::OUTLINE, 0.3f), Fade(ui::FILL, 0.4f));
            int tw = MeasureText(label, 20);
            DrawText(label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + 16), 20, GRAY);
            return false;
        }
        return uiEnabled && UiButton(r, label, 20);
    };

    // QUICK MATCH first: it is the shortest path from "I want to play" to
    // playing, and it is what someone with no opinion about rooms wants. Burying
    // it one screen deep in the browser made the browser mandatory.
    const bool online = networked && connected;
    if (row("QUICK MATCH", online))   action = TitleAction::QuickMatch;
    if (row("FIND A MATCH", online))  action = TitleAction::FindMatch;
    if (row("CUSTOM MATCH", online))  action = TitleAction::CustomMatch;
    if (row("LOCAL MATCH", true))     action = TitleAction::LocalMatch;

    if (networked && !connected) {
        UiTextCentered("CONNECTING TO SERVER...", screenWidth, (int)y + 6, 18, GRAY);
    } else if (!networked) {
        UiTextCentered("OFFLINE - START WITH A SERVER URL TO PLAY ONLINE",
                       screenWidth, (int)y + 6, 16, GRAY);
    }

    // One bottom row rather than a stacked LEADERBOARD: the fourth destination
    // (QUICK MATCH) took the vertical space that used to sit in. It is not a
    // match option, it is a place to look, so the router is allowed to offer it.
    //
    // It used to be networked-only, because the only table was the server's.
    // There is a LOCAL board now, so there is always something behind it - and
    // the offline player, who has no other high-score display at all, is the one
    // who benefits most.
    float by = screenHeight - 100.0f;
#if defined(__EMSCRIPTEN__)
    // No QUIT in a browser tab: breaking the loop would leave a dead canvas with
    // no way back. Closing the tab is the platform's own quit.
    if (uiEnabled && UiButton({325, by, 160, 44}, "CONTROLS", 16)) action = TitleAction::Controls;
    if (uiEnabled && UiButton({515, by, 160, 44}, "SCORES", 16))
        action = TitleAction::Leaderboard;
#else
    if (uiEnabled && UiButton({325, by, 110, 44}, "CONTROLS", 16)) action = TitleAction::Controls;
    if (uiEnabled && UiButton({445, by, 110, 44}, "SCORES", 16))
        action = TitleAction::Leaderboard;
    if (uiEnabled && UiButton({565, by, 110, 44}, "QUIT", 16))     action = TitleAction::Quit;
#endif

    DrawVolumeSlider(s, screenWidth, screenHeight, uiEnabled);
    return action;
}

//MARK: LOCAL
// Offline setup. Its own options and its own map size, deliberately not shared
// with an online room - a local game and a networked match are different things.
enum class LocalAction { None, Start, Options, Back };

struct LocalResult {
    LocalAction action = LocalAction::None;
};

inline LocalResult DrawLocalSetup(ShellState& s, const std::vector<Player>& players,
                                  const std::string& myName, MatchOptions& opt,
                                  int screenWidth, int screenHeight, bool uiEnabled) {
    LocalResult out;
    UiTextCentered("LOCAL MATCH", screenWidth, 110, 48, RAYWHITE);
    UiTextCentered("OFFLINE - YOU AND THE BOTS", screenWidth, 170, 18, GRAY);

    const float bottom = DrawRosterPanel(s, players, /*myIndex*/ 0, myName, opt,
                                         /*networked*/ false, "", MatchKind::Custom,
                                         /*top*/ 240.0f);

    const float startY = bottom + 26.0f;
    if (DrawMapSelector(opt, startY, uiEnabled, /*canStart*/ true))
        out.action = LocalAction::Start;

    const float by = startY + 70.0f;
    if (uiEnabled && UiButton({300, by, 180, 44}, "OPTIONS")) out.action = LocalAction::Options;
    if (uiEnabled && UiButton({520, by, 180, 44}, "BACK"))    out.action = LocalAction::Back;

    DrawVolumeSlider(s, screenWidth, screenHeight, uiEnabled);
    return out;
}

//MARK: CUSTOM
// Set up a room you are about to host: what it is called, who can find it, and
// the rules it opens with. The rules stay editable in the lobby afterwards, so
// they are never hidden from the people they apply to.
//
// Its options are the ONLINE set, never the local one. Retuning your offline
// game must not silently reconfigure a room you host, or the other way round.
enum class CustomAction { None, Create, Options, Back };

inline CustomAction DrawCustomSetup(ShellState& s, int screenWidth, int screenHeight,
                                    bool connected, bool uiEnabled) {
    CustomAction action = CustomAction::None;

    UiTextCentered("CUSTOM MATCH", screenWidth, 110, 48, RAYWHITE);
    UiTextCentered("A ROOM YOU HOST - YOU SET THE RULES AND PRESS START",
                   screenWidth, 170, 18, GRAY);

    UiTextCentered("MATCH NAME", screenWidth, 250, 18, ui::OUTLINE);
    UiTextField({300, 276, 400, 44}, s.customName, s.customNameFocused,
                MATCH_NAME_MAX_CHARS, 22);

    UiTextCentered("INVITE ONLY", screenWidth, 342, 18, ui::OUTLINE);
    if (uiEnabled) UiToggle({470, 368, 100, 26}, s.customPrivate);
    // The toggle is ONLY visibility. A custom room is host-run either way - you
    // set the rules and you press START - which is exactly what #107 separated.
    UiTextCentered(s.customPrivate
                       ? "Hidden from FIND A MATCH. Share the code to let people in."
                       : "Listed in FIND A MATCH for anyone to join.",
                   screenWidth, 406, 16, GRAY);
    UiTextCentered("You host it either way.", screenWidth, 428, 15, GRAY);

    if (uiEnabled && UiButton({350, 462, 300, 44}, "MATCH RULES", 18))
        action = CustomAction::Options;
    if (uiEnabled && connected && UiButton({350, 522, 300, 52}, "CREATE", 20))
        action = CustomAction::Create;
    if (uiEnabled && UiButton({350, 588, 300, 44}, "BACK", 18))
        action = CustomAction::Back;

    // CREATE is on THIS screen, so its refusal has to be on this screen too. The
    // per-address room budget is the one that actually turns up in practice, and
    // without this the button looked broken rather than refused.
    DrawRefusalLine(s, screenWidth, 646, GetTime());

    DrawVolumeSlider(s, screenWidth, screenHeight, uiEnabled);
    return action;
}

//MARK: LOBBY
// A room you are standing in. What it offers depends entirely on how that room is
// governed (#107) - offer a control the server refuses and it reads as the game
// ignoring you.
//
//   custom, you host it  OPTIONS, a map size, START
//   custom, you do not   who you are waiting on
//   official             the head count, then the countdown. No START at all:
//                        the server rejects options/start/endmatch from every
//                        connection in an official room.
enum class LobbyAction { None, Start, Options, Controls, Leaderboard, CopyInvite, Leave };

struct LobbyResult {
    LobbyAction action = LobbyAction::None;
};

inline LobbyResult DrawLobby(ShellState& s, const std::vector<Player>& players,
                             int myIndex, const std::string& myName,
                             MatchOptions& opt, int screenWidth, int screenHeight,
                             bool ready, float autoStartIn, bool uiEnabled) {
    LobbyResult out;
    const bool official = (s.inMatchKind == MatchKind::Official);

    UiTextCentered(official ? "OFFICIAL MATCH" : "MATCH LOBBY", screenWidth, 100, 44, RAYWHITE);
    if (!s.inMatchCode.empty()) {
        UiTextCentered(TextFormat("CODE  %s", s.inMatchCode.c_str()), screenWidth, 152, 22,
                       official ? GRAY : ui::OUTLINE);
        // The code is the whole invite for an invite-only room, so it needs to be
        // gettable, not just readable off the screen.
        if (uiEnabled && UiButton({620, 148, 130, 30}, "COPY INVITE", 14))
            out.action = LobbyAction::CopyInvite;
        if (!official)
            UiTextCentered(s.copyNotice.empty() ? "SHARE IT TO INVITE ANYONE"
                                                : s.copyNotice.c_str(),
                           screenWidth, 184, 15,
                           s.copyNotice.empty() ? GRAY : ui::OUTLINE);
    }

    const int  hostSlot = HostSlot(players);
    const bool amHost   = (myIndex >= 0 && myIndex == hostSlot);

    const float bottom = DrawRosterPanel(s, players, myIndex, myName, opt, /*networked*/ true,
                                         s.inMatchCode, s.inMatchKind, /*top*/ 220.0f);
    const float startY = bottom + 26.0f;

    if (!ready) {
        // "MATCH IN PROGRESS" is what this used to say, and it was a guess: a
        // room can be full and still sitting in its lobby. It is also no longer
        // the whole story - since E2 the server keeps us connected with no slot
        // instead of hanging up, and our hello retries until one opens, so the
        // wait is real and ends by itself. Say that.
        const char* msg = s.serverFull ? "NO FREE SLOT - WAITING FOR ONE TO OPEN..."
                        : myIndex >= 0 ? "JOINING..." : "CONNECTING...";
        UiTextCentered(msg, screenWidth, (int)startY + 14, 20, GRAY);
    } else if (official) {
        // No host to wait on: this room waits on a HEAD COUNT, then on a clock.
        // Saying neither left a window in which the room had silently committed
        // to starting and nobody in it could tell.
        int humans = 0;
        for (const Player& p : players) if (p.isConnected && !p.isBot) humans++;
        // The room's own threshold, not the compile-time one: a preset may ask
        // for more (opt.minHumansToStart), and a hard-coded 2 here would leave
        // such a room counting down to nothing.
        const int needed = opt.minHumansToStart - humans;
        if (autoStartIn > 0.0f) {
            UiTextCentered(TextFormat("MATCH STARTING IN %d...", (int)ceilf(autoStartIn)),
                           screenWidth, (int)startY + 14, 26, RAYWHITE);
        } else if (needed > 0) {
            UiTextCentered(needed == 1 ? "WAITING FOR 1 MORE PLAYER..."
                                       : TextFormat("WAITING FOR %d MORE PLAYERS...", needed),
                           screenWidth, (int)startY + 14, 20, GRAY);
        } else {
            // Head count met but the countdown has not reached us yet - one
            // packet's worth of gap, not an error.
            UiTextCentered("STARTING...", screenWidth, (int)startY + 14, 20, GRAY);
        }
    } else if (amHost) {
        if (DrawMapSelector(opt, startY, uiEnabled, /*canStart*/ true))
            out.action = LobbyAction::Start;
    } else if (hostSlot >= 0) {
        std::string hostName = !players[hostSlot].name.empty()
            ? players[hostSlot].name : TextFormat("PLAYER %d", hostSlot + 1);
        UiTextCentered(TextFormat("Waiting for %s to start the game.", hostName.c_str()),
                       screenWidth, (int)startY + 14, 20, GRAY);
        UiTextCentered(TextFormat("MAP  %s", opt.mapSize.c_str()),
                       screenWidth, (int)startY + 40, 16, ui::OUTLINE);
    } else {
        // Custom room with nobody hosting it: only possible in the gap between a
        // host leaving and the next state packet.
        UiTextCentered("WAITING FOR A HOST...", screenWidth, (int)startY + 14, 20, GRAY);
    }

    // A GRID, filled in order, not a column. OPTIONS is conditional, so a column
    // would leave a hole wherever it is hidden - and four stacked rows under a
    // full eight-slot roster run off the bottom of the window.
    float bx = 300.0f, by = startY + (amHost && !official ? 70.0f : 44.0f);
    auto button = [&](const char* label) {
        Rectangle r = {bx, by, 180, 44};
        if (bx < 400.0f) { bx = 520.0f; } else { bx = 300.0f; by += 52.0f; }
        return uiEnabled && UiButton(r, label);
    };
    // OPTIONS reconfigures the whole match, so it is the host's alone - and in an
    // official room it belongs to nobody, because the preset is the point.
    if (amHost && !official && button("OPTIONS")) out.action = LobbyAction::Options;
    if (button("LEADERBOARD")) out.action = LobbyAction::Leaderboard;
    if (button("CONTROLS"))    out.action = LobbyAction::Controls;
    if (button("LEAVE"))       out.action = LobbyAction::Leave;

    DrawVolumeSlider(s, screenWidth, screenHeight, uiEnabled);
    return out;
}
