// profile_test.cpp - round-trip and hostile-input coverage for profile.h.
//
// Build:  g++ -std=c++17 -I../server -I/opt/homebrew/include profile_test.cpp -o profile_test
// (-I../server picks up the headless raylib stub, so no raylib install is needed)
// (or via `make -C test` / test/run.sh)
//
// Runs against a redirected HOME so it never touches the real profile.

// constants.h uses raylib's Color without including raylib itself - every TU in
// this project pulls raylib.h in first, and so does this one. -I../server makes
// that the headless stub, so the test builds with no raylib install.
#include "raylib.h"
#include "../profile.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

// fopen returning NULL (no directory, no permission) previously walked straight
// into fputs and took the process down, which reported as a segfault instead of
// as the missing directory it actually was.
static void WriteFile(const std::string& path, const char* body) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("  FAIL: could not open %s for writing\n", path.c_str()); ++failures; return; }
    std::fputs(body, f);
    std::fclose(f);
}
#define CHECK(cond, what) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", what); ++failures; } \
    else         { std::printf("  ok:   %s\n", what); } \
} while (0)

int main() {
    // Unbuffered: the output has to survive a crash, or a failure tells us nothing.
    setvbuf(stdout, nullptr, _IONBF, 0);
    // Sandbox HOME so the real ~/Library/Application Support is untouched.
    const char* env = std::getenv("PROFILE_TEST_HOME");
    if (!env || !*env) { std::printf("PROFILE_TEST_HOME not set\n"); return 2; }
    // Copy before setenv: it may reallocate the environment block, which frees
    // the string getenv() just handed back.
    const std::string sandbox = env;
    setenv("HOME", sandbox.c_str(), 1);

    const std::string path = profile::StoragePath();
    std::printf("storage path: %s\n", path.c_str());
    CHECK(!path.empty(), "storage path resolves under HOME");
    CHECK(path.find(sandbox) == 0, "storage path stays inside HOME (never beside the binary)");
    std::remove(path.c_str());

    // --- first launch: no file -----------------------------------------
    std::printf("\nfirst launch\n");
    profile::Load();
    const std::string id1 = profile::Get().clientId;
    CHECK(id1.size() == 36, "clientId is a 36-char UUID");
    CHECK(id1[14] == '4', "UUID version nibble is 4");
    CHECK(id1[19]=='8'||id1[19]=='9'||id1[19]=='a'||id1[19]=='b', "UUID variant nibble is 10xx");
    CHECK(profile::Get().name == "PLAYER", "name defaults to PLAYER");
    CHECK(profile::Get().token.empty(), "token starts empty (D3 fills it)");

    // --- a save, then a reload -----------------------------------------
    std::printf("\nsave and reload\n");
    profile::Get().name = "MIKE";
    profile::Get().masterVolumeDb = -12.5f;
    profile::Get().lastLocalOptions.mapSize = "XL";
    profile::Get().lastLocalOptions.numPlayers = 6;
    profile::Get().lastLocalOptions.friendlyFire = !MatchOptions{}.friendlyFire;
    profile::Get().lastMatch = "7QK2";
    // The two rule sets must not be the same values, or "they round-trip
    // independently" below would pass even if one overwrote the other.
    profile::Get().lastCustomOptions.mapSize    = "SMALL";
    profile::Get().lastCustomOptions.numPlayers = 3;
    profile::Get().lastCustomOptions.speedBoost = 1.75f;
    CHECK(profile::Save(), "Save() writes when something changed");
    CHECK(!profile::Save(), "Save() is a no-op when nothing changed");

    profile::Load();
    CHECK(profile::Get().clientId == id1,      "clientId survives a relaunch");
    CHECK(profile::Get().name == "MIKE",       "name survives a relaunch");
    CHECK(profile::Get().masterVolumeDb < -12.4f && profile::Get().masterVolumeDb > -12.6f,
                                               "volume survives a relaunch");
    CHECK(profile::Get().lastLocalOptions.mapSize == "XL",     "map survives a relaunch");
    CHECK(profile::Get().lastLocalOptions.numPlayers == 6,     "player count survives a relaunch");
    CHECK(profile::Get().lastLocalOptions.friendlyFire != MatchOptions{}.friendlyFire,
                                                               "a toggle survives a relaunch");
    CHECK(profile::Get().lastMatch == "7QK2",  "last match code survives a relaunch");
    CHECK(profile::Get().lastCustomOptions.mapSize == "SMALL",   "custom map survives a relaunch");
    CHECK(profile::Get().lastCustomOptions.numPlayers == 3,      "custom roster survives a relaunch");
    CHECK(profile::Get().lastCustomOptions.speedBoost > 1.74f
       && profile::Get().lastCustomOptions.speedBoost < 1.76f,   "custom slider survives a relaunch");
    // The whole point of two bundles: hosting a fast 3-player room must not
    // retune the 6-player XL arena you practise in alone.
    CHECK(profile::Get().lastLocalOptions.mapSize == "XL"
       && profile::Get().lastCustomOptions.mapSize == "SMALL",
          "local and custom rules stay independent");

    // --- garbage on disk ------------------------------------------------
    std::printf("\ncorrupt file\n");
    WriteFile(path, "{not json at all");
    profile::Load();
    CHECK(profile::Get().clientId.size() == 36, "unparseable file yields a fresh id, not a crash");
    CHECK(profile::Get().name == "PLAYER",      "unparseable file falls back to defaults");

    // --- hand-edited hostile values ------------------------------------
    std::printf("\nhand-edited values\n");
    WriteFile(path, "{\"name\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
                    "\"clientId\":\"nope\",\"volumeDb\":9999,"
                    "\"options\":{\"players\":400,\"map\":\"MOON\",\"botDiff\":\"hard\","
                    "\"explRadius\":1000,\"fuelBurn\":-5},"
                    "\"customOptions\":{\"players\":-9,\"jetThrust\":50}}");
    profile::Load();
    const profile::Profile& p = profile::Get();
    CHECK(p.name.size() <= PLAYER_NAME_MAX_CHARS,   "over-long name is clamped");
    CHECK(p.clientId.size() == 36,                  "malformed clientId is replaced");
    CHECK(p.masterVolumeDb <= 0.0f,                 "out-of-range volume is clamped");
    CHECK(p.lastLocalOptions.numPlayers <= GAMESPACE_NUMBER_OF_PLAYERS, "roster is clamped to the cap");
    CHECK(p.lastLocalOptions.mapSize == "MEDIUM",   "unknown map falls back to the default");
    CHECK(p.lastLocalOptions.botDifficulty == MatchOptions{}.botDifficulty,
                                                    "wrong-typed number is ignored, not thrown on");
    CHECK(p.lastLocalOptions.explosionRadiusScale <= 4.0f, "out-of-range scale is clamped");
    CHECK(p.lastLocalOptions.fuelConsumption >= 0,  "negative fuel burn is clamped");
    CHECK(p.lastCustomOptions.numPlayers >= 1,      "custom roster is clamped too, not just local");
    CHECK(p.lastCustomOptions.jetpackThrust <= 2.0f,"custom scale is clamped too");

    // --- a missing key is not a reset -----------------------------------
    std::printf("\npartial file (a future build's field set, minus keys)\n");
    WriteFile(path, "{\"clientId\":\"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\",\"name\":\"SOLO\"}");
    profile::Load();
    CHECK(profile::Get().clientId == "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", "known keys load");
    CHECK(profile::Get().name == "SOLO",                                     "known keys load");
    CHECK(profile::Get().lastLocalOptions.mapSize == MatchOptions{}.mapSize, "absent keys keep defaults");
    // A profile written before custom rooms were remembered has no
    // "customOptions" at all. That must read as "no habit yet", not as garbage.
    CHECK(profile::Get().lastCustomOptions.mapSize == MatchOptions{}.mapSize,
          "a file with no customOptions loads the defaults");
    CHECK(profile::Get().lastCustomOptions.numPlayers == MatchOptions{}.numPlayers,
          "...across every field, not just the first");

    // --- ids do not repeat ----------------------------------------------
    std::printf("\nuuid uniqueness\n");
    {
        std::string a = profile::MakeUuidV4(), b = profile::MakeUuidV4(), c = profile::MakeUuidV4();
        CHECK(a != b && b != c && a != c, "consecutive UUIDs differ");
    }

    std::remove(path.c_str());
    std::printf("\n%s\n", failures ? "FAILURES" : "all profile checks passed");
    return failures ? 1 : 0;
}
