// Do the client and the server agree on every match rule?
//
// The rule bundle crosses the wire as JSON in both directions, written and read
// by different code on each side:
//
//   client -> server   wire.h writes "start"/"options" with a real JSON library;
//                      the server reads them by string search (options_json.h)
//   server -> client   the server writes the "opt" echo by hand (2 decimals);
//                      wire.h reads it back with the JSON library
//
// All four walk one list now (VisitMatchOptions, options.h). This is the test
// that they really do - that a rule set on one side arrives on the other - and
// it compares bundles through that same list, so a rule added there is covered
// here without anyone remembering to add it.
//
//   g++ -std=c++17 -O2 -I server -I . -I/opt/homebrew/include -DPLATFORMZ_SERVER \
//       server/test/options_wire_test.cpp -o /tmp/ow && /tmp/ow
#include "wire.h"            // the CLIENT's writer and reader
#include "options_json.h"    // the SERVER's reader and writer

#include <cstdio>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string& what) {
    printf("  %s %s\n", ok ? "ok:  " : "FAIL:", what.c_str());
    if (!ok) failures++;
}

// A bundle as (key, printed value) rows - which is how two bundles are compared
// without this file keeping a field list of its own.
struct Rows {
    std::vector<std::pair<std::string, std::string>> rows;
    void integer(const char* k, int v)                { rows.push_back({k, std::to_string(v)}); }
    void real   (const char* k, float v)              { char b[32]; snprintf(b, sizeof b, "%.2f", v); rows.push_back({k, b}); }
    void flag   (const char* k, bool v)               { rows.push_back({k, v ? "true" : "false"}); }
    void text   (const char* k, const std::string& v) { rows.push_back({k, v}); }
};
static Rows rowsOf(const MatchOptions& o) { Rows r; VisitMatchOptions(o, r); return r; }

static void same(const MatchOptions& got, const MatchOptions& want, const char* what) {
    Rows g = rowsOf(got), w = rowsOf(want);
    bool ok = true;
    for (size_t i = 0; i < w.rows.size(); ++i)
        if (g.rows[i] != w.rows[i]) {
            ok = false;
            printf("        %s: got %s, want %s\n", w.rows[i].first.c_str(),
                   g.rows[i].second.c_str(), w.rows[i].second.c_str());
        }
    check(ok, what);
}

// Every rule moved off its default, to a legal value, so a rule that fails to
// cross shows up as "arrived as the default". Flips each flag, steps each
// number, picks a different arena.
struct Nudge {
    void integer(const char*, int& v)   { v = v > 2 ? v - 1 : v + 1; }
    void real   (const char*, float& v) { v += 0.25f; }
    void flag   (const char*, bool& v)  { v = !v; }
    void text   (const char*, std::string& v) { v = (v == "SMALL") ? "XL" : "SMALL"; }
};

static MatchOptions serverReads(const std::string& msg, MatchOptions base) {
    OptionJsonReader reader{msg};
    VisitMatchOptions(base, reader);
    return base;
}

int main() {
    const MatchOptions defaults;
    MatchOptions tuned;
    { Nudge n; VisitMatchOptions(tuned, n); }
    {   // the nudge itself must be legal, or the comparisons below prove nothing
        MatchOptions c = tuned; ClampOptions(c);
        same(c, tuned, "the test bundle is inside every legal range");
        Rows a = rowsOf(tuned), b = rowsOf(defaults);
        bool allMoved = true;
        for (size_t i = 0; i < a.rows.size(); ++i) if (a.rows[i] == b.rows[i]) allMoved = false;
        check(allMoved, "and differs from the default in every single rule");
    }

    printf("the rule list\n");
    {
        Rows r = rowsOf(defaults);
        std::set<std::string> keys;
        for (auto& kv : r.rows) keys.insert(kv.first);
        check(keys.size() == r.rows.size(), "every rule has its own key");
        check(r.rows.size() == 17, "seventeen rules - update CLAUDE.md's count with this number");
    }

    printf("client -> server\n");
    {
        same(serverReads(serializeStart(tuned), MatchOptions{}), tuned,
             "START carries every rule");
        same(serverReads(serializeOptions(tuned), defaults), tuned,
             "an OPTIONS update carries every rule");
        same(serverReads("{\"type\":\"start\"}", MatchOptions{}), defaults,
             "a START with no rules in it means the defaults");
        MatchOptions want = tuned; want.speedBoost = 1.5f;
        same(serverReads("{\"type\":\"options\",\"boost\":1.5}", tuned), want,
             "an OPTIONS message with one key changes only that rule");
        same(serverReads("{\"type\":\"options\",\"map\":\"\"}", tuned), tuned,
             "an empty arena name is not a change");
    }

    printf("server -> client\n");
    {
        std::string s = "{\"type\":\"state\",\"opt\":{";
        OptionJsonWriter w{s};
        VisitMatchOptions(tuned, w);
        s += "}}";
        nlohmann::json j = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
        check(!j.is_discarded(), "the server's echo is valid JSON");
        MatchOptions got;
        if (!j.is_discarded()) {
            wire_detail::OptionKeyReader reader{j["opt"]};
            VisitMatchOptions(got, reader);
        }
        same(got, tuned, "the echo carries every rule back (to 2 decimals)");
    }

    printf("what the server will accept\n");
    {
        MatchOptions wild = serverReads(
            "{\"type\":\"start\",\"nplayers\":0,\"boost\":1000,\"xradius\":-3,"
            "\"fburn\":999,\"maxbots\":200,\"map\":\"NOWHERE\"}", MatchOptions{});
        ClampOptions(wild);   // what Match::setOptions does to everything coming in
        check(wild.numPlayers == (int)OPT_RANGE_NUM_PLAYERS.min, "0 players becomes the minimum");
        check(wild.speedBoost == OPT_RANGE_SPEED_BOOST.max, "a speed boost of 1000 becomes the slider's max");
        check(wild.explosionRadiusScale == OPT_RANGE_EXPLOSION_RADIUS.min, "a negative blast radius becomes the minimum");
        check(wild.fuelConsumption == (int)OPT_RANGE_FUEL_CONSUMPTION.max, "fuel burn is capped");
        check(wild.maxBots == (int)OPT_RANGE_MAX_BOTS.max, "maxBots is capped");
        check(wild.mapSize == defaults.mapSize, "an unknown arena falls back to the default");
    }

    printf("\n%s\n", failures ? "FAILURES" : "client and server agree on every rule");
    return failures ? 1 : 0;
}
