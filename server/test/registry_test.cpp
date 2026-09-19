// Exercises MatchRegistry directly. A2 adds the registry but clients cannot yet
// reach a room by code (that is A3), so this is how its mechanics get tested:
// capacity, code uniqueness, reaping and its graces, preset seeding, and the
// governance a room's KIND implies.
//
//   g++ -std=c++17 -O2 -I server -I . -I/opt/homebrew/include -DPLATFORMZ_SERVER \
//       server/test/registry_test.cpp -o /tmp/registry_test && /tmp/registry_test
#include "registry.h"
#include <cassert>
#include <cstdio>
#include <set>
#include <string>

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}
using Clock = Match::Clock;

static std::shared_ptr<Match> make(MatchRegistry& r, std::string& code,
                                   bool priv = false,
                                   MatchKind kind = MatchKind::Custom) {
    MatchRegistry::CreateResult why;
    return r.Create("room", "DEFAULT", kind, priv, "", code, why);
}

int main() {
    printf("capacity\n");
    {
        MatchRegistry r{3};
        std::string c;
        for (int i = 0; i < 3; i++) check(make(r, c) != nullptr, "create under cap");
        MatchRegistry::CreateResult why;
        check(r.Create("x", "DEFAULT", MatchKind::Custom, false, "", c, why) == nullptr,
              "refused at cap");
        check(why == MatchRegistry::CreateResult::AtCapacity, "reason is AtCapacity");
        check(r.Size() == 3, "size stays at cap");
    }

    printf("codes\n");
    {
        MatchRegistry r{200};
        std::set<std::string> seen;
        std::string c;
        for (int i = 0; i < 200; i++) { make(r, c); seen.insert(c); }
        check(seen.size() == 200, "200 codes, all distinct");
        bool clean = true;
        for (const auto& s : seen)
            for (char ch : s) if (ch=='O'||ch=='0'||ch=='I'||ch=='1') clean = false;
        check(clean, "no ambiguous characters (O/0/I/1)");
        check(seen.begin()->size() == 4, "codes are 4 chars");
    }

    printf("preset seeding\n");
    {
        MatchRegistry r{4};
        std::string c;
        auto m = make(r, c);
        const MatchPreset& p = MatchPresetByName("DEFAULT");
        check(m->pendingPlayers.load() == p.options.numPlayers, "options copied from preset");
        // The map is inside `options` now, and the match holds it as an index
        // into mapSizeOrder rather than as three loose numbers.
        check(m->pendingMap.load() == MapSizeIndex(p.options.mapSize), "map copied from preset");
        check(MapSizeName(m->pendingMap.load()) == p.options.mapSize, "...and round-trips by name");
        check(MatchPresetByName("NOPE").options.numPlayers == p.options.numPlayers,
              "unknown preset falls back to DEFAULT");
    }

    printf("every preset is dialable in the UI\n");
    {
        // The third consumer of the ranges in options.h, and the one that had NO
        // check at all: a preset sets fields directly, bypassing both the OPTIONS
        // sliders and the profile's clamp. So a preset could hold a value no
        // player can select and nothing would say so - the only thing stopping it
        // was a "// slider max is 4.0" comment written from memory.
        //
        // Also catches the reverse: narrow a range and any preset now outside it
        // fails here rather than being silently clamped on someone's machine.
        for (const auto& entry : matchOptionPresets) {
            // Not a structured binding: capturing one in the lambda below is a
            // C++20 extension and this builds as C++17.
            const std::string&  name = entry.first;
            const MatchOptions& o    = entry.second.options;
            auto inRange = [&](const OptionRange& r, float v, const char* field) {
                const std::string what = name + "." + field + " = " + std::to_string(v) +
                                         " is within [" + std::to_string(r.min) + ", " +
                                         std::to_string(r.max) + "]";
                check(r.holds(v), what.c_str());
            };
            inRange(OPT_RANGE_NUM_PLAYERS,         (float)o.numPlayers,    "numPlayers");
            inRange(OPT_RANGE_BOT_DIFFICULTY,      o.botDifficulty,        "botDifficulty");
            inRange(OPT_RANGE_WALL_ELASTICITY,     o.wallElasticity,       "wallElasticity");
            inRange(OPT_RANGE_PLATFORM_ELASTICITY, o.platformElasticity,   "platformElasticity");
            inRange(OPT_RANGE_SPEED_BOOST,         o.speedBoost,           "speedBoost");
            inRange(OPT_RANGE_ROCKET_SPEED,        o.rocketSpeedScale,     "rocketSpeedScale");
            inRange(OPT_RANGE_EXPLOSION_RADIUS,    o.explosionRadiusScale, "explosionRadiusScale");
            inRange(OPT_RANGE_JETPACK_THRUST,      o.jetpackThrust,        "jetpackThrust");
            inRange(OPT_RANGE_FUEL_CONSUMPTION,    (float)o.fuelConsumption, "fuelConsumption");
            inRange(OPT_RANGE_FUEL_REGEN,          (float)o.fuelRegenPct,  "fuelRegenPct");
            // A preset's arena has to be a real one - MapSizeIndex would quietly
            // substitute MEDIUM for a typo, so the room would run, on the wrong map.
            const std::string arena = name + ".mapSize \"" + o.mapSize + "\" is a real arena";
            check(mapSizePresets.find(o.mapSize) != mapSizePresets.end(), arena.c_str());
        }

        // And the defaults themselves, which are what an untouched modal shows.
        MatchOptions d;
        MatchOptions clamped = d;
        ClampOptions(clamped);
        check(clamped.numPlayers == d.numPlayers && clamped.botDifficulty == d.botDifficulty &&
              clamped.speedBoost == d.speedBoost && clamped.fuelConsumption == d.fuelConsumption &&
              clamped.fuelRegenPct == d.fuelRegenPct && clamped.mapSize == d.mapSize,
              "MatchOptions{} is already in range - clamping it changes nothing");
    }

    // The point of #107: governance comes from KIND, and visibility is a
    // separate, independent fact. The four combinations must all be expressible,
    // and the two flags must always agree with each other - a locked room that
    // cannot start itself has nothing able to begin it.
    printf("governance follows kind, not visibility\n");
    {
        MatchRegistry r{8};
        std::string c, offPubCode, cusPubCode;

        auto offPub = make(r, offPubCode, /*priv*/false, MatchKind::Official);
        check(offPub->optionsLocked && offPub->autoStart,
              "official: locked and self-starting");

        auto offPriv = make(r, c, /*priv*/true, MatchKind::Official);
        check(offPriv->optionsLocked && offPriv->autoStart,
              "official stays official when hidden");

        // The combination the old code could not express at all: public, but
        // host-run. Before the split, public MEANT locked + auto-start.
        auto cusPub = make(r, cusPubCode, /*priv*/false, MatchKind::Custom);
        check(!cusPub->optionsLocked && !cusPub->autoStart,
              "public CUSTOM room keeps host control");

        auto cusPriv = make(r, c, /*priv*/true, MatchKind::Custom);
        check(!cusPriv->optionsLocked && !cusPriv->autoStart,
              "custom stays host-run when hidden");

        // The browser has to be able to tell them apart, and both are public, so
        // the kind cannot be inferred from the listing's other fields.
        MatchKind listedOfficial = MatchKind::Custom, listedCustom = MatchKind::Official;
        for (const MatchListing& row : r.List()) {
            if (row.code == offPubCode) listedOfficial = row.kind;
            if (row.code == cusPubCode) listedCustom   = row.kind;
        }
        check(listedOfficial == MatchKind::Official, "listing reports the official room as official");
        check(listedCustom   == MatchKind::Custom,   "listing reports the custom room as custom");
    }

    printf("listing\n");
    {
        MatchRegistry r{8};
        std::string pubCode, privCode;
        make(r, pubCode, /*priv*/false);
        MatchRegistry::CreateResult why;
        r.Create("secret", "DEFAULT", MatchKind::Custom, /*priv*/true, "abcd", privCode, why);
        check(r.List().size() == 1, "private rooms are not advertised");
        check(r.List(true).size() == 2, "...but are listed when asked for");
        check(r.List()[0].code == pubCode, "public room appears by code");
        check(r.List()[0].players == 0 && r.List()[0].joinable, "empty room is joinable");
    }

    printf("reaping\n");
    {
        MatchRegistry r{8};
        std::string c;
        auto m = make(r, c);
        auto now = Clock::now();
        check(r.Reap(now).empty(), "not reaped inside its grace");

        auto late = now + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double>(MATCH_EMPTY_GRACE_IDLE_SEC + 1));
        // occupied rooms survive regardless
        m->connectedCount.store(1);
        check(r.Reap(late).empty(), "occupied room survives past the grace");
        // ...and go when they empty
        m->connectedCount.store(0);
        auto later = late + std::chrono::duration_cast<Clock::duration>(
                                std::chrono::duration<double>(MATCH_EMPTY_GRACE_IDLE_SEC + 1));
        check(r.Reap(later).size() == 1, "empty room reaped after its grace");
        check(r.Size() == 0, "registry now empty");
    }

    printf("live rooms get the longer grace\n");
    {
        MatchRegistry r{8};
        std::string c;
        auto m = make(r, c);
        m->gamePhase = Phase::PLAYING;
        auto now = Clock::now();
        auto mid = now + std::chrono::duration_cast<Clock::duration>(
                             std::chrono::duration<double>(MATCH_EMPTY_GRACE_IDLE_SEC + 1));
        check(r.Reap(mid).empty(), "PLAYING room survives the idle grace");
        auto past = now + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double>(MATCH_EMPTY_GRACE_LIVE_SEC + 1));
        check(r.Reap(past).size() == 1, "...and goes at the live grace");
    }

    printf("pinning\n");
    {
        MatchRegistry r{8};
        std::string c;
        make(r, c);
        r.Pin(c);
        auto forever = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                          std::chrono::duration<double>(MATCH_MAX_AGE_SEC * 2));
        check(r.Reap(forever).empty(), "pinned room is never reaped");
        check(r.Size() == 1, "...and stays in the registry");
    }

    printf("\n%s\n", failures ? "FAILURES" : "all registry checks passed");
    return failures ? 1 : 0;
}
