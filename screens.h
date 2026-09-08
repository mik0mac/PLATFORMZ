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

    // Set when WE asked to move rooms (join / quick / create), so the welcome
    // that lands us somewhere new can be told apart from the one every client
    // gets on connect - and from the one that comes back after LEAVE, which
    // would otherwise bounce us straight into the room we just left.
    bool joinPending = false;
    bool roomChanged = false;   // a requested move completed; the screen acts on it

    // ---- Custom match setup (CUSTOM) ------------------------------------
    std::string customName;                 // room name, defaulted from the player's
    bool        customNameFocused = false;
    bool        customPrivate = false;      // invite-only: hidden from the browser

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
        // Slots 0..previewCount-1 are all occupied (human or bot), so they draw as
        // contiguous rows.
        for (int i = 0; i < previewCount; ++i) {
            int  ry  = (int)(box.y + headerH + i * rowH);
            bool you = (i == myIndex);
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

// The four map-size presets, as a row of START buttons. Shared by LOCAL and by a
// custom room's host. Returns the preset name that was pressed, or "".
inline std::string DrawMapSizeRow(float y, bool uiEnabled) {
    const char* names[] = {"SMALL", "MEDIUM", "LARGE", "XL"};
    for (int i = 0; i < 4; ++i) {
        Rectangle r = {110.0f + i * 200.0f, y, 180.0f, 50.0f};
        if (uiEnabled && UiButton(r, names[i])) return names[i];
    }
    return std::string();
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
    // (QUICK MATCH) took the vertical space that used to sit in. SCORES is
    // networked-only - the table is owned and persisted by the server, so offline
    // there is nothing behind it. It is not a match option, it is a place to look,
    // so the router is allowed to offer it.
    float by = screenHeight - 100.0f;
#if defined(__EMSCRIPTEN__)
    // No QUIT in a browser tab: breaking the loop would leave a dead canvas with
    // no way back. Closing the tab is the platform's own quit.
    if (uiEnabled && UiButton({325, by, 160, 44}, "CONTROLS", 16)) action = TitleAction::Controls;
    if (online && uiEnabled && UiButton({515, by, 160, 44}, "SCORES", 16))
        action = TitleAction::Leaderboard;
#else
    if (uiEnabled && UiButton({325, by, 110, 44}, "CONTROLS", 16)) action = TitleAction::Controls;
    if (online && uiEnabled && UiButton({445, by, 110, 44}, "SCORES", 16))
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
    std::string mapSize;   // Start: which preset
};

inline LocalResult DrawLocalSetup(ShellState& s, const std::vector<Player>& players,
                                  const std::string& myName, const MatchOptions& opt,
                                  int screenWidth, int screenHeight, bool uiEnabled) {
    LocalResult out;
    UiTextCentered("LOCAL MATCH", screenWidth, 110, 48, RAYWHITE);
    UiTextCentered("OFFLINE - YOU AND THE BOTS", screenWidth, 170, 18, GRAY);

    const float bottom = DrawRosterPanel(s, players, /*myIndex*/ 0, myName, opt,
                                         /*networked*/ false, "", MatchKind::Custom,
                                         /*top*/ 240.0f);

    const float startY = bottom + 26.0f;
    std::string picked = DrawMapSizeRow(startY, uiEnabled);
    if (!picked.empty()) { out.action = LocalAction::Start; out.mapSize = picked; }

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
    UiTextField({300, 276, 400, 44}, s.customName, s.customNameFocused, 24, 22);

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
    std::string mapSize;   // Start: which preset
};

inline LobbyResult DrawLobby(ShellState& s, const std::vector<Player>& players,
                             int myIndex, const std::string& myName,
                             const MatchOptions& opt, int screenWidth, int screenHeight,
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

    // Host is whatever slot the SERVER flagged. We do not recompute it: the host
    // is the room's creator, not the lowest slot, and an official room has none.
    int hostSlot = -1;
    for (int i = 0; i < (int)players.size(); ++i)
        if (players[i].isHost) { hostSlot = i; break; }
    const bool amHost = (myIndex >= 0 && myIndex == hostSlot);

    const float bottom = DrawRosterPanel(s, players, myIndex, myName, opt, /*networked*/ true,
                                         s.inMatchCode, s.inMatchKind, /*top*/ 220.0f);
    const float startY = bottom + 26.0f;

    if (!ready) {
        const char* msg = s.serverFull ? "MATCH IN PROGRESS - WAITING FOR A SLOT..."
                        : myIndex >= 0 ? "JOINING..." : "CONNECTING...";
        UiTextCentered(msg, screenWidth, (int)startY + 14, 20, GRAY);
    } else if (official) {
        // No host to wait on: this room waits on a HEAD COUNT, then on a clock.
        // Saying neither left a window in which the room had silently committed
        // to starting and nobody in it could tell.
        int humans = 0;
        for (const Player& p : players) if (p.isConnected && !p.isBot) humans++;
        const int needed = PUBLIC_MIN_PLAYERS - humans;
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
        std::string picked = DrawMapSizeRow(startY, uiEnabled);
        if (!picked.empty()) { out.action = LobbyAction::Start; out.mapSize = picked; }
    } else if (hostSlot >= 0) {
        std::string hostName = !players[hostSlot].name.empty()
            ? players[hostSlot].name : TextFormat("PLAYER %d", hostSlot + 1);
        UiTextCentered(TextFormat("Waiting for %s to start the game.", hostName.c_str()),
                       screenWidth, (int)startY + 14, 20, GRAY);
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
