// platform_steam.cpp - platform.h's Steam implementation (F4, #95).
//
// Compiled ONLY into the Steam edition (CMake -DPLATFORMZ_STEAM=ON), which also
// defines PLATFORMZ_STEAM so platform.h declares these instead of its stand-ins.
// Every other build never sees this file.
//
// Its own translation unit for the same reason as net_native.cpp: Steamworks
// headers pull in platform headers that collide with raylib.h on Windows. So this
// file must never include raylib.h or any game header that does - which is also
// why it logs with printf rather than raylib's TraceLog.
//
// Deliberately NOT called: SteamAPI_RestartAppIfNecessary. It relaunches the game
// through Steam when it was started some other way - the right call for a shipped
// Steam build that wants the overlay guaranteed, and the wrong one during
// development, where we launch from the build folder with steam_appid.txt beside
// us. Decide it at release, with the real App ID.

#include "platform.h"

#include <steam/steam_api.h>

#include <cstdio>
#include <string>

namespace {

bool        g_active = false;   // SteamAPI_InitEx succeeded and has not been shut down
std::string g_pendingJoin;      // a friend's "Join game" not yet taken by the game
std::string g_published;        // the room code our rich presence currently offers
bool        g_publishedKnown = false;

// Steam delivers events to objects that register for them; STEAM_CALLBACK does
// the registering in the constructor, so this is created only after a
// successful init and destroyed before shutdown.
struct SteamEvents {
    // A friend chose "Join game" on us (or accepted an invite) while we were
    // running. When we were NOT running, Steam launches us with the same text
    // on the command line instead, and main.cpp's --match parsing takes it.
    STEAM_CALLBACK(SteamEvents, OnJoinRequested, GameRichPresenceJoinRequested_t);
};

void SteamEvents::OnJoinRequested(GameRichPresenceJoinRequested_t* e) {
    // Untrusted: it is whatever the friend's game published. main.cpp passes it
    // through platform::ParseJoinString, which accepts only "--match CODE".
    g_pendingJoin.assign(e->m_rgchConnect);
}

SteamEvents* g_events = nullptr;

} // namespace

namespace platform {

bool Init() {
    if (g_active) return true;
    SteamErrMsg err = {0};
    const ESteamAPIInitResult r = SteamAPI_InitEx(&err);
    if (r != k_ESteamAPIInitResult_OK) {
        // Normal, not fatal: Steam not running, or this copy not launched by it
        // and no steam_appid.txt beside it. The game plays exactly as the
        // website build does.
        std::printf("STEAM: not available (%d: %s) - playing without it\n", (int)r, err);
        return false;
    }
    g_active = true;
    g_events = new SteamEvents();
    std::printf("STEAM: ready (app %u)\n", SteamUtils()->GetAppID());
    return true;
}

void Update() {
    if (g_active) SteamAPI_RunCallbacks();   // delivers OnJoinRequested, and keeps the overlay alive
}

void Shutdown() {
    if (!g_active) return;
    SteamFriends()->ClearRichPresence();     // nobody should be offered a room we have left
    delete g_events;
    g_events = nullptr;
    SteamAPI_Shutdown();
    g_active = false;
}

bool Active() { return g_active; }

std::string TakeJoinRequest() {
    std::string s;
    s.swap(g_pendingJoin);
    return s;
}

void SetJoinableRoom(const std::string& code) {
    if (!g_active) return;
    if (g_publishedKnown && code == g_published) return;   // only reach Steam on a change
    // "connect" is the key Steam looks for: while it is set, friends see "Join
    // game" on us, and choosing it hands them this exact text. Empty deletes it.
    const bool ok = SteamFriends()->SetRichPresence("connect", code.empty() ? nullptr : JoinStringFor(code).c_str());
    if (code.empty()) std::printf("STEAM: no room to offer friends%s\n", ok ? "" : " (Steam refused)");
    else              std::printf("STEAM: offering room %s to friends%s\n", code.c_str(), ok ? "" : " (Steam refused)");
    g_published      = code;
    g_publishedKnown = true;
}

} // namespace platform
