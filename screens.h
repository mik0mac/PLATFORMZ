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
                // where a greyed FULL explains itself.
                UiPanel(joinBtn, Fade(ui::OUTLINE, 0.3f), Fade(ui::FILL, 0.4f));
                int tw = MeasureText("FULL", 16);
                DrawText("FULL", (int)(joinBtn.x + (joinBtn.width - tw) / 2),
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
