// platform.h - the storefront layer (F4, #95): what the game asks of whoever
// distributed it. Today that is Steam or nobody.
//
// Two builds from one source tree (the decision on #95): the Steam build is
// compiled with PLATFORMZ_STEAM=1 and gets the Steamworks implementation in
// platform_steam.cpp; every other build - the website download, the Mac handout,
// the browser - gets the stand-ins at the bottom of this file, which do nothing,
// and links nothing of Valve's. The game calls the same functions either way and
// never asks which build it is.
//
// Deliberately narrow. Matchmaking, rooms, the high-score board and the network
// all stay on our own server, because the browser has no Steam and one shared
// world beats two (see docs/matchmaking-plan.md, F4). Steam is used for what only
// it can do: the overlay, and friends joining friends.
//
// Nothing here may include a Steamworks header: steam_api.h pulls in platform
// headers that collide with raylib.h on Windows, exactly as winsock did (see
// net_native.cpp). The Steam calls live in their own translation unit.

#pragma once

#include <string>
#include <cctype>

namespace platform {

//MARK: Invites
// A friend's "Join game" reaches us as a short string we chose ourselves: the
// one SetJoinableRoom() publishes. It is the same text as the command-line flag,
// "--match CODE", so the two ways an invite can arrive are one path:
//   - the game is NOT running: Steam launches it with that text appended to the
//     command line, and main.cpp's existing --match parsing reads it;
//   - the game IS running: TakeJoinRequest() hands over the same text, and
//     ParseJoinString() below reads it.
inline std::string JoinStringFor(const std::string& code) { return "--match " + code; }

// The room code in a join string, or "" when there isn't a usable one. The
// string came from another player's machine, so it is read strictly: the
// "--match" token, then one code of letters and digits, at most 8 of them
// (codes are 4 today), uppercased - nothing else is accepted.
inline std::string ParseJoinString(const std::string& s) {
    const std::string flag = "--match";
    size_t i = 0;
    auto skipSpace = [&]() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; };
    skipSpace();
    if (s.compare(i, flag.size(), flag) != 0) return std::string();
    i += flag.size();
    if (i >= s.size() || !std::isspace((unsigned char)s[i])) return std::string();
    skipSpace();
    std::string code;
    while (i < s.size() && !std::isspace((unsigned char)s[i])) {
        const unsigned char c = (unsigned char)s[i++];
        if (!std::isalnum(c) || c > 127) return std::string();
        code.push_back((char)std::toupper(c));
    }
    skipSpace();
    if (i != s.size()) return std::string();          // trailing junk
    if (code.empty() || code.size() > 8) return std::string();
    return code;
}

#if defined(PLATFORMZ_STEAM)
//MARK: Steam build (platform_steam.cpp)
// Call Init() once, after the window exists (the overlay hooks the window's
// rendering), Update() once a frame, and Shutdown() on the way out.
bool Init();
void Update();
void Shutdown();

// True while Steam is up and the game is attached to it. False is normal in a
// Steam build too - Steam not running, or launched outside it in development -
// and every function below is then a harmless no-op.
bool Active();

// A friend's "Join game" that arrived while we were running, as the raw join
// string, or "" when there is none. Taking it clears it.
std::string TakeJoinRequest();

// Tell friends which room we are in, so they see "Join game" on us; "" when we
// are in none (a menu, or a LOCAL match) takes it away. Cheap to call every
// frame - it only reaches Steam when the room changes.
void SetJoinableRoom(const std::string& code);

#else
//MARK: Every other build
inline bool        Init()                            { return false; }
inline void        Update()                          {}
inline void        Shutdown()                        {}
inline bool        Active()                          { return false; }
inline std::string TakeJoinRequest()                 { return std::string(); }
inline void        SetJoinableRoom(const std::string&) {}
#endif

} // namespace platform
