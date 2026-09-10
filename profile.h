// profile.h
//
// The player's persistent local profile: display name, a stable per-install
// clientId, master volume, and the last LOCAL match rules. Written as small JSON
// to a per-user location the game owns.
//
// This is the project's ONLY persistence layer on the client side, and it is
// deliberately tiny. Three rules shape everything below:
//
//   1. It never fails a launch. A missing file, an unreadable directory, a
//      truncated write, JSON that is not JSON - every one of those lands on
//      defaults and a freshly minted clientId. There is no error path that
//      reaches the player, because there is nothing here worth interrupting a
//      game over.
//   2. It is a STORAGE format, not the wire format. wire.h is free to change
//      shape whenever WELCOME_BIN_VERSION bumps, because both ends ship
//      together; a file on disk has to be readable by a build written a year
//      from now. So the mapping is spelled out field by field with a default
//      for every missing key, rather than reusing writeOptionKeys().
//   3. It never writes next to the binary. Inside the signed .app,
//      Contents/MacOS/ is CODE as far as codesign is concerned (see the
//      cwd-anchoring note in CLAUDE.md) - a file dropped there breaks the
//      notarized handout.
//
//MARK: What clientId is, and what it is not
// clientId is a UUID this client generates once and then keeps. Its contract is
// "the same value as last launch", nothing more. The player owns the file, so
// they can edit it, copy it to another machine, or delete it for a fresh one -
// it is an identifier, not a credential. That is fine for what reads it today
// (D2's 15-second slot restore) and is exactly why D3 exists: the server signs a
// token, and `token` below is the field it lands in.
//
//MARK: Web storage - localStorage, not cookies
// A cookie rides along on every request the browser makes to the origin, which
// would ship the id to the web host on every asset fetch for no reason, and the
// whole domain shares a ~4 KB budget. localStorage is a private per-origin
// drawer that goes nowhere unless we send it, holds megabytes, and survives
// reloads and restarts.
//
// It is best-effort, and the ways it evaporates are worth knowing:
//   - Per ORIGIN. The same game served from github.io and from your own domain
//     are two different drawers, so two different clientIds.
//   - Per BROWSER, per DEVICE. Chrome and Safari on one Mac are two players.
//   - A private/incognito window discards it when the window closes, and Safari
//     there refuses writes outright (the setItem throws - caught below).
//   - Safari's tracking prevention evicts script-written storage after 7 days
//     without a visit, so a player who takes a fortnight off returns as new.
//   - "Clear browsing data" wipes it.
// So a web clientId is stable across sessions, usually, and that is the most a
// browser will promise. Anything that must survive harder needs the account the
// project has deliberately chosen not to have.

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <random>
#include <cstdio>

#include "options.h"   // MatchOptions
#include "constants.h" // MASTER_VOLUME_MIN_DB, PLAYER_NAME_MAX_CHARS

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#elif defined(_WIN32)
#include <cstdlib>     // getenv
#include <direct.h>    // _mkdir
#else
#include <cstdlib>     // getenv
#include <sys/stat.h>  // mkdir
#endif

namespace profile {

// Local rather than raymath's Clamp: constants.h needs raylib's Color but
// nothing here needs raymath, and test/profile_test.cpp builds against the
// server's raylib stub with no raylib installed at all. One three-token helper
// is cheaper than a dependency that would break that.
inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

//MARK: Format version
// Bumped only if a future build has to MIGRATE an old file rather than just
// ignore keys it doesn't know. Additive changes need no bump: every read below
// supplies a default, so an old file simply lacks the new key.
constexpr int PROFILE_FORMAT_VERSION = 1;

//MARK: The profile
struct Profile {
    std::string name;              // display name - the player changes this freely
    std::string clientId;          // UUIDv4, minted once, never rewritten
    std::string token;             // server-issued identity token (D3). Empty until then.
    float       masterVolumeDb = 0.0f;  // 0 dB = full scale (see audio.h)
    MatchOptions lastLocalOptions;      // the offline match's rules
    std::string lastServer;        // last server URL played on   (recorded; unread today)
    std::string lastMatch;         // last room code joined       (recorded; unread today)
};

//MARK: UUID
// Version-4 UUID from std::random_device. Deliberately NOT random.h's
// RandomFloat/mt19937: that generator is shared, seeded for gameplay, and its
// whole point is reproducible-ish arena layout. An install identifier wants the
// opposite, and it is minted exactly once per install so the cost is irrelevant.
inline std::string MakeUuidV4() {
    std::random_device rd;
    std::uniform_int_distribution<int> hex(0, 15);
    static const char* digits = "0123456789abcdef";
    std::string u;
    u.reserve(36);
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { u += '-'; continue; }
        if (i == 14) { u += '4'; continue; }                       // version nibble
        if (i == 19) { u += digits[(hex(rd) & 0x3) | 0x8]; continue; } // variant 10xx
        u += digits[hex(rd)];
    }
    return u;
}

//MARK: Where it lives
#if !defined(__EMSCRIPTEN__)
// mkdir every component of `path`, ignoring "already exists". Recursive because
// the parent is not guaranteed: ~/Library/Application Support exists on a normal
// macOS account but not in a sandbox or a freshly created one, and ~/.config may
// be absent entirely. Creating only the leaf leaves Save() silently failing
// forever, which is exactly the kind of quiet nothing this file must not do.
inline void MakeDirs(const std::string& path) {
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != sep) continue;
        const std::string part = path.substr(0, i);
#if defined(_WIN32)
        _mkdir(part.c_str());
#else
        ::mkdir(part.c_str(), 0700);   // 0700: nobody else's business
#endif
    }
}

// Returns the directory, creating it if needed; empty when there is no home to
// put it in (in which case the caller simply doesn't persist - see rule 1).
inline std::string StorageDir() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !*appdata) return std::string();
    std::string dir = std::string(appdata) + "\\PLATFORMZ";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home || !*home) return std::string();
    std::string dir = std::string(home) + "/Library/Application Support/PLATFORMZ";
#else
    // Linux/BSD: XDG. Not a client target today, but the server box compiles
    // these same headers, so leaving it undefined would be a trap.
    const char* xdg  = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string base = (xdg && *xdg) ? std::string(xdg)
                     : (home && *home) ? std::string(home) + "/.config"
                     : std::string();
    if (base.empty()) return std::string();
    std::string dir = base + "/platformz";
#endif
    MakeDirs(dir);
    return dir;
}

inline std::string StoragePath() {
    const std::string dir = StorageDir();
    if (dir.empty()) return std::string();
#if defined(_WIN32)
    return dir + "\\profile.json";
#else
    return dir + "/profile.json";
#endif
}
#endif // !__EMSCRIPTEN__

//MARK: Raw read / write
#if defined(__EMSCRIPTEN__)
// Returns the byte length needed (including the NUL) and fills `out` when it
// fits; -1 when there is nothing stored or storage is unavailable. Safari in a
// private window THROWS on both of these rather than returning null, so both are
// wrapped - an uncaught exception here would take the frame down.
EM_JS(int, PlatformzProfileRead, (char* out, int cap), {
    try {
        var s = window.localStorage.getItem('platformz.profile');
        if (s === null || s === undefined) return -1;
        var need = lengthBytesUTF8(s) + 1;
        if (need <= cap) stringToUTF8(s, out, cap);
        return need;
    } catch (e) { return -1; }
});

EM_JS(int, PlatformzProfileWrite, (const char* json), {
    try { window.localStorage.setItem('platformz.profile', UTF8ToString(json)); return 1; }
    catch (e) { return 0; }   // quota exceeded, or storage disabled entirely
});
#endif

inline std::string ReadRaw() {
#if defined(__EMSCRIPTEN__)
    std::string buf(4096, '\0');
    int need = PlatformzProfileRead(&buf[0], (int)buf.size());
    if (need < 0) return std::string();
    if (need > (int)buf.size()) {          // grew past the first guess - retry exactly
        buf.assign((size_t)need, '\0');
        need = PlatformzProfileRead(&buf[0], (int)buf.size());
        if (need < 0 || need > (int)buf.size()) return std::string();
    }
    return std::string(buf.c_str());
#else
    const std::string path = StoragePath();
    if (path.empty()) return std::string();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string out;
    char chunk[1024];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) out.append(chunk, n);
    std::fclose(f);
    return out;
#endif
}

inline bool WriteRaw(const std::string& json) {
#if defined(__EMSCRIPTEN__)
    return PlatformzProfileWrite(json.c_str()) != 0;
#else
    const std::string path = StoragePath();
    if (path.empty()) return false;
    // Write-then-rename, so a crash or a full disk mid-write leaves the previous
    // profile intact instead of a half-written file that parses as garbage and
    // silently resets the player's name and id. rename() is atomic within a
    // filesystem, and the temp file is a sibling so it always is one.
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const size_t wrote = std::fwrite(json.data(), 1, json.size(), f);
    const bool   ok    = (wrote == json.size()) && (std::fclose(f) == 0);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
#if !defined(_WIN32)
    // 0600 explicitly rather than whatever the umask allowed. The directory is
    // already 0700, so this is belt-and-braces today - but D3 lands a signed
    // identity token in this file, and a world-readable secret is the kind of
    // thing that is much easier to get right before it is a secret.
    ::chmod(path.c_str(), 0600);
#endif
    return true;
#endif
}

//MARK: Serialize
// Field by field on purpose - see rule 2. Every read supplies a default, so a
// file written by an older build (or a hand-edited one missing keys) loads
// cleanly instead of throwing.
inline std::string Serialize(const Profile& p) {
    const MatchOptions& o = p.lastLocalOptions;
    nlohmann::json j = {
        {"version",  PROFILE_FORMAT_VERSION},
        {"name",     p.name},
        {"clientId", p.clientId},
        {"token",    p.token},
        {"volumeDb", p.masterVolumeDb},
        {"lastServer", p.lastServer},
        {"lastMatch",  p.lastMatch},
        {"options", {
            {"map",         o.mapSize},
            {"players",     o.numPlayers},
            {"botDiff",     o.botDifficulty},
            {"wallElast",   o.wallElasticity},
            {"platElast",   o.platformElasticity},
            {"speedBoost",  o.speedBoost},
            {"rocketSpeed", o.rocketSpeedScale},
            {"explRadius",  o.explosionRadiusScale},
            {"jetThrust",   o.jetpackThrust},
            {"fuelBurn",    o.fuelConsumption},
            {"fuelRegen",   o.fuelRegenPct},
            {"walls",       o.wallsEnabled},
            {"rocketPhys",  o.rocketsObeyPhysics},
            {"friendlyFire",o.friendlyFire},
            {"coast",       o.coastMode},
        }},
    };
    return j.dump();
}

// Fills `p` from `raw`, leaving any field the file doesn't carry at whatever `p`
// already held. Returns false if there was nothing usable to read, which is the
// caller's cue to mint a fresh clientId.
inline bool Deserialize(const std::string& raw, Profile& p) {
    if (raw.empty()) return false;
    nlohmann::json j = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    // Every getter is guarded by a type check, not just a presence check: a
    // hand-edited file with "players": "eight" must not throw.
    auto str = [&](const nlohmann::json& o, const char* k, std::string& dst) {
        auto it = o.find(k); if (it != o.end() && it->is_string()) dst = it->get<std::string>();
    };
    auto flt = [&](const nlohmann::json& o, const char* k, float& dst) {
        auto it = o.find(k); if (it != o.end() && it->is_number()) dst = it->get<float>();
    };
    auto integer = [&](const nlohmann::json& o, const char* k, int& dst) {
        auto it = o.find(k); if (it != o.end() && it->is_number_integer()) dst = it->get<int>();
    };
    auto boolean = [&](const nlohmann::json& o, const char* k, bool& dst) {
        auto it = o.find(k); if (it != o.end() && it->is_boolean()) dst = it->get<bool>();
    };

    str(j, "name",       p.name);
    str(j, "clientId",   p.clientId);
    str(j, "token",      p.token);
    str(j, "lastServer", p.lastServer);
    str(j, "lastMatch",  p.lastMatch);
    flt(j, "volumeDb",   p.masterVolumeDb);

    auto oi = j.find("options");
    if (oi != j.end() && oi->is_object()) {
        const nlohmann::json& o = *oi;
        MatchOptions& m = p.lastLocalOptions;
        str    (o, "map",          m.mapSize);
        integer(o, "players",      m.numPlayers);
        flt    (o, "botDiff",      m.botDifficulty);
        flt    (o, "wallElast",    m.wallElasticity);
        flt    (o, "platElast",    m.platformElasticity);
        flt    (o, "speedBoost",   m.speedBoost);
        flt    (o, "rocketSpeed",  m.rocketSpeedScale);
        flt    (o, "explRadius",   m.explosionRadiusScale);
        flt    (o, "jetThrust",    m.jetpackThrust);
        integer(o, "fuelBurn",     m.fuelConsumption);
        integer(o, "fuelRegen",    m.fuelRegenPct);
        boolean(o, "walls",        m.wallsEnabled);
        boolean(o, "rocketPhys",   m.rocketsObeyPhysics);
        boolean(o, "friendlyFire", m.friendlyFire);
        boolean(o, "coast",        m.coastMode);
    }
    return true;
}

//MARK: Sanity
// A profile is a file the player can edit, so treat every loaded value as
// untrusted input and clamp it into the range the UI can actually represent.
// Without this, a hand-typed "players": 400 would reach setPlayerCount().
inline void Sanitize(Profile& p) {
    if (p.name.size() > PLAYER_NAME_MAX_CHARS) p.name.resize(PLAYER_NAME_MAX_CHARS);
    std::string clean;
    for (char c : p.name) if (c >= 32 && c <= 125) clean += c;   // same rule as the wire's clampName
    p.name = clean;

    // A clientId that is not the shape we mint is not one of ours; replace it
    // rather than propagating it into a future hello.
    if (p.clientId.size() != 36) p.clientId = MakeUuidV4();

    p.masterVolumeDb = Clampf(p.masterVolumeDb, MASTER_VOLUME_MIN_DB, 0.0f);

    MatchOptions& m = p.lastLocalOptions;
    if (mapSizePresets.find(m.mapSize) == mapSizePresets.end()) m.mapSize = MatchOptions{}.mapSize;
    m.numPlayers          = (int)Clampf((float)m.numPlayers, 1.0f, (float)GAMESPACE_NUMBER_OF_PLAYERS);
    m.botDifficulty       = Clampf(m.botDifficulty, 0.0f, BOT_DIFFICULTY);
    m.wallElasticity      = Clampf(m.wallElasticity, 0.0f, 1.0f);
    m.platformElasticity  = Clampf(m.platformElasticity, 0.0f, 1.0f);
    m.speedBoost          = Clampf(m.speedBoost, 1.0f, 2.0f);
    m.rocketSpeedScale    = Clampf(m.rocketSpeedScale, 1.0f, 2.0f);
    m.explosionRadiusScale= Clampf(m.explosionRadiusScale, 1.0f, 4.0f);
    m.jetpackThrust       = Clampf(m.jetpackThrust, 1.0f, 2.0f);
    m.fuelConsumption     = (int)Clampf((float)m.fuelConsumption, 0.0f, 100.0f);
    m.fuelRegenPct        = (int)Clampf((float)m.fuelRegenPct, 0.0f, 100.0f);
}

//MARK: The one instance
// A single process-wide profile, because there is exactly one player at this
// keyboard. Load() is idempotent; everything else assumes it has run.
inline Profile& Get() { static Profile p; return p; }

// Remembers what was last written, so Save() can skip a no-op write. On the web
// that matters more than it looks: localStorage writes are synchronous and hit
// the main thread, so writing an unchanged blob every frame would show up as
// jitter in a 60 Hz loop.
inline std::string& LastWritten() { static std::string s; return s; }

// Reads the stored profile, or starts a fresh one. Safe to call before the
// window exists - it touches no raylib state.
inline void Load() {
    Profile& p = Get();
    p = Profile{};                       // defaults first, so a partial file fills in
    p.name = "PLAYER";
    const std::string raw = ReadRaw();
    const bool had = Deserialize(raw, p);
    if (!had || p.clientId.empty()) p.clientId = MakeUuidV4();
    Sanitize(p);
    LastWritten() = had ? raw : std::string();
}

// Writes if anything actually changed. Returns true when a write happened.
inline bool Save() {
    const std::string out = Serialize(Get());
    if (out == LastWritten()) return false;
    if (!WriteRaw(out)) return false;
    LastWritten() = out;
    return true;
}

// Call once per frame with the live values. Persists at most once every
// AUTOSAVE_INTERVAL seconds, and only when something differs from the last
// write - so dragging the volume slider costs one write when the drag settles,
// not one per frame.
//
// The autosave is what makes the web build work at all: closing a tab runs no
// teardown, so a save that only happened on exit would never happen there.
constexpr double AUTOSAVE_INTERVAL = 2.0;

inline void Autosave(double now) {
    static double nextAt = 0.0;
    if (now < nextAt) return;
    nextAt = now + AUTOSAVE_INTERVAL;
    Save();
}

} // namespace profile
