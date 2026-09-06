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
#include "ui.h"          // the immediate-mode widgets the screens are built from

#include <string>
#include <vector>

//MARK: Screens
// TITLE doubles as the lobby in networked play; BROWSE and LOBBY split that in
// two once the match browser lands (C2/C3).
enum class GameScreen { TITLE, BROWSE, COUNTDOWN, PLAYING, GAME_OVER };

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
    bool showScores   = false;   // leaderboard popup (networked only)

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
    std::vector<MatchSummary> matches;      // current page, newest reply wins
    int   listCursor  = 0;                  // page we asked for
    int   listNext    = -1;                 // cursor for the next page, -1 = last
    int   listTotal   = 0;                  // rooms the server says exist
    int   browseScroll = 0;                 // first visible row
    double lastListAt = 0.0;                // GetTime() of the last refresh
    bool   awaitingList = false;            // a request is outstanding
    // The room we are actually in, straight from the welcome - not the code we
    // asked for. Quick match picks a room for us, and connecting with no room
    // named lands us in one we never chose, so only the server knows.
    std::string inMatchCode;
    MatchKind   inMatchKind = MatchKind::Custom;

    std::string joinCode;                   // JOIN CODE field contents
    bool        joinCodeFocused = false;
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

// Read-only: the server owns the table and pushes it on join and after every
// credited match, so there is nothing to refresh from here.
inline void DrawLeaderboardModal(ShellState& s, int screenWidth, bool wasOpen) {
    Rectangle m = {250, 140, 500, 420};
    UiModalChrome(m, "LEADERBOARD");
    if (s.leaderboard.empty()) {
        // Distinguish "nothing recorded yet" from a broken panel -
        // a fresh server with no score file lands here.
        UiTextCentered("No scores recorded yet.", screenWidth,
                       (int)m.y + 120, 20, GRAY);
    } else {
        int ly = (int)m.y + 60;
        for (size_t i = 0; i < s.leaderboard.size(); ++i) {
            // Rank and name left, score right-aligned inside the panel
            // so the numbers line up regardless of name length.
            const char* rank = TextFormat("%d. %s", (int)i + 1,
                                          s.leaderboard[i].name.c_str());
            const char* val  = TextFormat("%d", s.leaderboard[i].score);
            DrawText(rank, (int)m.x + 40, ly, 18, RAYWHITE);
            DrawText(val, (int)(m.x + m.width - 40 - MeasureText(val, 18)),
                     ly, 18, ui::OUTLINE);
            ly += 30;
        }
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
             1.0f, (float)GAMESPACE_NUMBER_OF_PLAYERS,
             s.sliderPlayersActive, 1.0f)) {
        opt.numPlayers = (int)s.optNumPlayersF; optChanged = true;
    }

    // BOT DIFFICULTY (continuous, 0.0..BOT_DIFFICULTY).
    DrawText("BOT DIFFICULTY", (int)lxL, y2, 18, RAYWHITE);
    valueAt(TextFormat("%.2f", opt.botDifficulty), lxL, y2);
    if (UiSlider({lxL, (float)(y2 + 26), colW, 22}, opt.botDifficulty,
             0.0f, BOT_DIFFICULTY, s.sliderDiffActive)) optChanged = true;

    // WALL ELASTICITY (players only; asteroids keep their constant).
    DrawText("WALL ELASTICITY", (int)lxL, y3, 18, RAYWHITE);
    valueAt(TextFormat("%.2f", opt.wallElasticity), lxL, y3);
    if (UiSlider({lxL, (float)(y3 + 26), colW, 22}, opt.wallElasticity,
             0.0f, 1.0f, s.sliderWElastActive)) optChanged = true;

    // PLATFORM ELASTICITY (players only; asteroids keep their constant).
    DrawText("PLATFORM ELASTICITY", (int)lxL, y4, 18, RAYWHITE);
    valueAt(TextFormat("%.2f", opt.platformElasticity), lxL, y4);
    if (UiSlider({lxL, (float)(y4 + 26), colW, 22}, opt.platformElasticity,
             0.0f, 1.0f, s.sliderPElastActive)) optChanged = true;

    // SPEED BOOST (walk + jetpack speed/accel, and rocket speed).
    DrawText("SPEED BOOST", (int)lxL, y5, 18, RAYWHITE);
    valueAt(TextFormat("%.1fx", opt.speedBoost), lxL, y5);
    if (UiSlider({lxL, (float)(y5 + 26), colW, 22}, opt.speedBoost,
             1.0f, 2.0f, s.sliderBoostActive)) optChanged = true;

    // --- Right column ---
    // ROCKET VELOCITY (on top of SPEED BOOST).
    DrawText("ROCKET VELOCITY", (int)lxR, y1, 18, RAYWHITE);
    valueAt(TextFormat("%.1fx", opt.rocketSpeedScale), lxR, y1);
    if (UiSlider({lxR, (float)(y1 + 26), colW, 22}, opt.rocketSpeedScale,
             1.0f, 2.0f, s.sliderRSpeedActive)) optChanged = true;

    // JETPACK THRUST (on top of SPEED BOOST; jetpack only).
    DrawText("JETPACK THRUST", (int)lxR, y2, 18, RAYWHITE);
    valueAt(TextFormat("%.1fx", opt.jetpackThrust), lxR, y2);
    if (UiSlider({lxR, (float)(y2 + 26), colW, 22}, opt.jetpackThrust,
             1.0f, 2.0f, s.sliderJThrustActive)) optChanged = true;

    // FUEL CONSUMPTION (direct units/sec out of the 100-unit tank).
    DrawText("FUEL CONSUMPTION (%)", (int)lxR, y3, 18, RAYWHITE);
    valueAt(TextFormat("%d/sec", (int)s.optFuelBurnF), lxR, y3);
    if (UiSlider({lxR, (float)(y3 + 26), colW, 22}, s.optFuelBurnF,
             0.0f, 100.0f, s.sliderFBurnActive, 1.0f)) {
        opt.fuelConsumption = (int)s.optFuelBurnF; optChanged = true;
    }

    // FUEL REGEN (percentage of the consumption rate; 100% = keeps pace).
    DrawText("FUEL REGEN (% of consmpt.)", (int)lxR, y4, 18, RAYWHITE);
    valueAt(TextFormat("%d/sec", (int)s.optFuelRegenF), lxR, y4);
    if (UiSlider({lxR, (float)(y4 + 26), colW, 22}, s.optFuelRegenF,
             0.0f, 100.0f, s.sliderFRegenActive, 1.0f)) {
        opt.fuelRegenPct = (int)s.optFuelRegenF; optChanged = true;
    }

    // EXPLOSION RADIUS (damage radius + blast visual; last in the
    // modal per its own domain, grouped away from the speed trio).
    DrawText("EXPLOSION RADIUS", (int)lxR, y5, 18, RAYWHITE);
    valueAt(TextFormat("%.1fx", opt.explosionRadiusScale), lxR, y5);
    if (UiSlider({lxR, (float)(y5 + 26), colW, 22}, opt.explosionRadiusScale,
             1.0f, 4.0f, s.sliderXRadiusActive)) optChanged = true;

    // Toggles: three across, label on its own line, a compact ON/OFF
    // control below (labels are long, so keep them off the control's
    // line). Each defaults to its constants.h value; applied at match
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

//MARK: BROWSE
// What the player asked for this frame. The screen reports intent only - main()
// owns the socket and decides what to send - so this stays free of networking.
enum class BrowseAction { None, Back, Refresh, Quick, Create, Join, Page };

struct BrowseResult {
    BrowseAction action = BrowseAction::None;
    std::string  code;       // Join: which room, from a row or the code field
    std::string  joinCode;   // Join: a private room's password, if one was typed
    int          cursor = 0; // Page: which page to ask for
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
    const int   rowsVisible = (int)(listH / rowH);

    UiTextCentered("FIND A MATCH", screenW, 80, 40, RAYWHITE);

    if (UiButton({listX + listW - 130.0f, listY - 46.0f, 130.0f, 34.0f}, "REFRESH", 18) && connected)
        out.action = BrowseAction::Refresh;

    // Total count, so a capped page is not mistaken for the whole world.
    DrawText(s.listTotal == 1 ? "1 MATCH" : TextFormat("%d MATCHES", s.listTotal),
             (int)listX, (int)listY - 40, 18, ui::OUTLINE);

    UiPanel({listX, listY, listW, listH});

    if (!connected) {
        UiTextCentered("NOT CONNECTED", screenW, (int)(listY + listH / 2 - 10), 20, GRAY);
    } else if (s.matches.empty()) {
        // "Empty" and "still loading" look identical unless you say which it is.
        UiTextCentered(s.awaitingList ? "LOOKING FOR MATCHES..." : "NO MATCHES YET",
                       screenW, (int)(listY + listH / 2 - 20), 20, ui::OUTLINE);
        if (!s.awaitingList)
            UiTextCentered("CREATE ONE, OR TRY QUICK MATCH",
                           screenW, (int)(listY + listH / 2 + 8), 16, GRAY);
    } else {
        for (int i = 0; i < rowsVisible; ++i) {
            const int idx = s.browseScroll + i;
            if (idx >= (int)s.matches.size()) break;
            const MatchSummary& m = s.matches[idx];
            const float ry = listY + 6.0f + i * rowH;

            DrawText(m.name.c_str(), (int)listX + 14, (int)ry + 10, 18, RAYWHITE);
            DrawText(TextFormat("%d/%d", m.players, m.maxPlayers),
                     (int)listX + 300, (int)ry + 10, 18, ui::OUTLINE);
            // Kind, not preset. "DEFAULT" in every row tells a player nothing,
            // where OFFICIAL vs CUSTOM tells them whether the rules are fixed and
            // the room starts itself, or whether somebody is running it and
            // decides both. Official is drawn brighter because it is the row you
            // can join and expect a game from without knowing anyone.
            const bool official = (m.kind == MatchKind::Official);
            DrawText(official ? "OFFICIAL" : "CUSTOM",
                     (int)listX + 380, (int)ry + 10, 16, official ? ui::OUTLINE : GRAY);
            DrawText(m.phase.c_str(),  (int)listX + 500, (int)ry + 10, 16,
                     m.phase == "playing" ? ui::OUTLINE : GRAY);

            Rectangle joinBtn = {listX + listW - 100.0f, ry + 4.0f, 86.0f, 30.0f};
            if (m.joinable) {
                if (UiButton(joinBtn, "JOIN", 16) && connected) {
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
                const char* why = (m.phase == "gameover") ? "ENDING"
                                : (m.players >= m.maxPlayers) ? "FULL"
                                : "CLOSED";   // shouldn't happen; better than lying
                UiPanel(joinBtn, Fade(ui::OUTLINE, 0.3f), Fade(ui::FILL, 0.4f));
                int tw = MeasureText(why, 16);
                DrawText(why, (int)(joinBtn.x + (joinBtn.width - tw) / 2),
                         (int)(joinBtn.y + 7), 16, GRAY);
            }
        }
    }

    // Paging appears only when there is more than one page to see.
    if (s.listNext >= 0 || s.listCursor > 0) {
        if (s.listCursor > 0 &&
            UiButton({listX, listY + listH + 8.0f, 90.0f, 30.0f}, "FIRST", 16) && connected) {
            out.action = BrowseAction::Page;
            out.cursor = 0;   // the wire protocol pages forward only
        }
        if (s.listNext >= 0 &&
            UiButton({listX + 100.0f, listY + listH + 8.0f, 90.0f, 30.0f}, "MORE", 16) && connected) {
            out.action = BrowseAction::Page;
            out.cursor = s.listNext;
        }
    }

    const float by = listY + listH + 56.0f;
    if (UiButton({listX, by, 170.0f, 44.0f}, "QUICK MATCH") && connected)
        out.action = BrowseAction::Quick;
    if (UiButton({listX + 190.0f, by, 170.0f, 44.0f}, "CREATE MATCH") && connected)
        out.action = BrowseAction::Create;

    // Join by code, for a private room whose code arrived out of band.
    DrawText("CODE", (int)listX + 390, (int)by + 14, 16, ui::OUTLINE);
    UiTextField({listX + 440.0f, by, 110.0f, 44.0f}, s.joinCode, s.joinCodeFocused, 8, 20);
    if (UiButton({listX + 560.0f, by, 100.0f, 44.0f}, "GO", 20) && connected && !s.joinCode.empty()) {
        out.action   = BrowseAction::Join;
        out.code     = s.joinCode;
        out.joinCode = s.joinCode;  // a private room's code doubles as its password
    }

    if (UiButton({listX + listW - 110.0f, by, 110.0f, 44.0f}, "BACK"))
        out.action = BrowseAction::Back;

    // Refusals and confirmations, fading after a few seconds so a stale message
    // is never mistaken for the current state.
    if (!s.browseStatus.empty()) {
        const double age = now - s.browseStatusAt;
        if (age < 6.0) {
            Color c = age > 4.0 ? Fade(RED, (float)((6.0 - age) / 2.0)) : RED;
            UiTextCentered(s.browseStatus.c_str(), screenW, (int)by + 60, 18, c);
        }
    }
    return out;
}
