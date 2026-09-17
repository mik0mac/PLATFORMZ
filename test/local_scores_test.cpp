// local_scores_test.cpp - the client's LOCAL high-score board.
//
// Build:  g++ -std=c++17 -I../server -I/opt/homebrew/include local_scores_test.cpp -o lst
// (-I../server picks up the headless raylib stub, so no raylib install is needed)
// (or via test/run.sh)
//
// Runs against a redirected HOME so it never touches the real board.
//
// Two things are worth testing here and neither is "does it store a number".
//
// The first is the CAP, because it is the only rule with teeth: the board is
// bounded by TrimRuns rather than by any size limit on the file, so if the trim
// is wrong the file grows forever on a machine nobody is watching. And the shape
// of the cap is what makes the board playable at all - nine bots would own every
// row of a board with one human on it.
//
// The second is that a hostile or damaged file cannot take the game down. The
// player owns this file; it is going to get hand-edited.

#include "raylib.h"
#include "../local_scores.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

#define CHECK(cond, what) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", what); ++failures; } \
    else         { std::printf("  ok:   %s\n", what); } \
} while (0)

static void WriteFile(const std::string& path, const char* body) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("  FAIL: could not open %s\n", path.c_str()); ++failures; return; }
    std::fputs(body, f);
    std::fclose(f);
}

// One run, the way endLocalMatch() records one.
static RunRow Run(const char* id, const char* name, int score, long long when) {
    RunRow r;
    r.score     = score;
    r.when      = when;
    r.map       = "MEDIUM";
    r.official  = false;
    r.matchName = "LOCAL MATCH";
    r.id        = id;
    r.name      = name;
    return r;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* env = std::getenv("PROFILE_TEST_HOME");
    if (!env || !*env) { std::printf("PROFILE_TEST_HOME not set\n"); return 2; }
    const std::string sandbox = env;
    setenv("HOME", sandbox.c_str(), 1);

    const std::string path = profile::StoragePathFor(localscores::STORAGE_LEAF);
    std::printf("storage path: %s\n", path.c_str());
    CHECK(!path.empty(), "storage path resolves under HOME");
    CHECK(path.find(sandbox) == 0, "storage path stays inside HOME (never beside the binary)");
    // A board of its own, NOT a key inside profile.json - clearing your scores
    // must not be able to cost you your name or your identity token.
    CHECK(path != profile::StoragePath(), "the board is its own file, apart from the profile");
    std::remove(path.c_str());

    // --- first launch: no file ------------------------------------------
    std::printf("\nfirst launch\n");
    localscores::Load();
    CHECK(localscores::Get().runs.empty(), "an absent file is an empty board, not a failure");
    CHECK(localscores::Get().top().empty(), "...and an empty board publishes no rows");
    RunRow best;
    CHECK(!localscores::Get().bestForPlayer(best), "...and there is no personal best to pin");

    // --- the cap: a human holds three rows -------------------------------
    // The rule that bounds the file. Five runs in, two of them are gone.
    std::printf("\nthe player holds at most three rows\n");
    localscores::Board& b = localscores::Get();
    for (int i = 0; i < 5; ++i)
        b.addRun(Run(localscores::LOCAL_PLAYER_ID, "MIKE", 100 + i * 10, 1000 + i));
    CHECK(b.runs.size() == 3, "five runs, three rows kept");
    CHECK(b.top()[0].score == 140 && b.top()[2].score == 120,
          "and they are the best three, ranked best-first");

    // --- the cap: every bot shares ONE row --------------------------------
    // The reason this matters more locally than on the server: a local match has
    // exactly one human in it, so bots with a row each would own the board.
    std::printf("\nall the bots hold one row between them\n");
    const char* botNames[] = {"GEOFF", "JURF", "BLIM", "NODD", "SKREE"};
    for (int i = 0; i < 5; ++i)
        b.addRun(Run(RUN_BOT_ID, botNames[i], 200 + i * 10, 2000 + i));
    int botRows = 0;
    for (const RunRow& r : b.runs) if (r.isBot()) botRows++;
    CHECK(botRows == 1, "five bot runs, one row");
    RunRow topBot;
    for (const RunRow& r : b.runs) if (r.isBot()) topBot = r;
    CHECK(topBot.score == 240, "and it is the best a bot managed");
    // The row still names the bot that set it - id and name are stored apart
    // precisely so a shared identity can still display an individual.
    CHECK(topBot.name == "SKREE", "...displayed under the bot that set it, not under the id");

    // --- ranking ----------------------------------------------------------
    std::printf("\nwhere a run landed\n");
    // The bots swept the top, so the player's best sits at 4th.
    CHECK(b.rankOf(localscores::LOCAL_PLAYER_ID, 1004) == 2,
          "the player's best run is ranked behind the bots' single row");
    CHECK(b.rankOf(localscores::LOCAL_PLAYER_ID, 999999) == 0,
          "a run that is not on the board ranks 0, not 1");
    // The case that makes rankOf look at the board instead of counting scores:
    // a fourth personal best beats nothing above it AND is trimmed away, so it
    // has no rank at all. Counting better scores would have called this #4.
    b.addRun(Run(localscores::LOCAL_PLAYER_ID, "MIKE", 5, 3000));
    CHECK(b.rankOf(localscores::LOCAL_PLAYER_ID, 3000) == 0,
          "a run trimmed away on arrival ranks 0, not 'just below the last row'");

    // --- round trip -------------------------------------------------------
    std::printf("\nsave and reload\n");
    CHECK(localscores::Save(), "a changed board writes");
    CHECK(!localscores::Save(), "an unchanged board does NOT write again");
    const size_t wasRows = b.runs.size();
    const int    wasTop  = b.top()[0].score;
    const std::string wasName = b.top()[0].name;
    localscores::Load();
    CHECK(localscores::Get().runs.size() == wasRows, "every row survived the round trip");
    CHECK(localscores::Get().top()[0].score == wasTop, "the top score survived");
    CHECK(localscores::Get().top()[0].name == wasName, "the frozen name survived");
    CHECK(localscores::Get().top()[0].map == "MEDIUM", "and so did the map it was set on");

    // --- renaming yourself does not rewrite history -----------------------
    // The asymmetry the design calls for: a RUN is an event and keeps the name it
    // was set under; a career total follows renames because it belongs to a
    // person. Only the run board exists locally, so only the freezing applies.
    std::printf("\nrenaming\n");
    localscores::Get().addRun(Run(localscores::LOCAL_PLAYER_ID, "MIKEY", 500, 4000));
    bool sawOld = false, sawNew = false;
    for (const RunRow& r : localscores::Get().runs) {
        if (r.isBot()) continue;
        if (r.name == "MIKE")  sawOld = true;
        if (r.name == "MIKEY") sawNew = true;
    }
    CHECK(sawNew, "the new run carries the new name");
    CHECK(sawOld, "...and an older run still carries the old one");

    // --- hostile input ----------------------------------------------------
    // The player owns this file. None of these may throw, and none may leave a
    // half-loaded board behind.
    std::printf("\nfiles the player edited\n");
    struct { const char* body; const char* what; } bad[] = {
        {"",                                          "an empty file"},
        {"not json at all",                           "a file that is not JSON"},
        {"[1,2,3]",                                   "JSON that is not an object"},
        {"{\"version\":1}",                           "an object with no runs array"},
        {"{\"runs\":\"nope\"}",                       "runs that is not an array"},
        {"{\"runs\":[1,2,3]}",                        "rows that are not objects"},
        {"{\"runs\":[{\"s\":\"lots\",\"id\":\"you\"}]}", "a score that is not a number"},
        {"{\"runs\":[{\"s\":10}]}",                   "a row with no identity"},
        {"{\"runs\":[{\"s\":10,\"id\":\"\"}]}",        "a row with an empty identity"},
    };
    for (const auto& t : bad) {
        WriteFile(path, t.body);
        localscores::Load();               // must not throw
        CHECK(localscores::Get().runs.empty(),
              (std::string(t.what) + " loads as an empty board").c_str());
    }

    // A file with one good row and one broken one keeps the good row: one bad
    // byte should cost one score, not the whole history.
    std::printf("\npartial damage\n");
    WriteFile(path,
        "{\"version\":1,\"runs\":["
        "{\"s\":900,\"w\":7,\"map\":\"XL\",\"m\":\"LOCAL MATCH\",\"id\":\"you\",\"n\":\"MIKE\"},"
        "{\"s\":\"broken\",\"id\":\"you\"},"
        "{\"s\":800,\"w\":8,\"id\":\"-BOT\",\"n\":\"GEOFF\"}]}");
    localscores::Load();
    CHECK(localscores::Get().runs.size() == 2, "the two readable rows loaded, the broken one did not");
    CHECK(localscores::Get().top()[0].score == 900, "and the survivors are ranked correctly");

    // A row claiming to be official must not become one: local runs are never
    // refereed, and a hand-edited flag is exactly how someone would try.
    std::printf("\nno local run is official\n");
    WriteFile(path, "{\"runs\":[{\"s\":10,\"id\":\"you\",\"n\":\"X\",\"official\":true}]}");
    localscores::Load();
    CHECK(!localscores::Get().runs.empty() && !localscores::Get().runs[0].official,
          "an official:true in the file is ignored");

    std::printf("\n");
    if (failures) { std::printf("%d CHECK(S) FAILED\n", failures); return 1; }
    std::printf("local_scores_test: all checks passed\n");
    return 0;
}
