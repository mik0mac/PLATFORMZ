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

#include <string>
#include <vector>

//MARK: Screens
// TITLE doubles as the lobby in networked play; BROWSE and LOBBY split that in
// two once the match browser lands (C2/C3).
enum class GameScreen { TITLE, COUNTDOWN, PLAYING, GAME_OVER };

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
