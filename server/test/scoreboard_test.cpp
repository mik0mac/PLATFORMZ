// server/test/scoreboard_test.cpp
//
// The all-time table, re-keyed onto the D3 identity (D4).
//
// The three behaviours that changed are all things the OLD table got wrong, and
// all three are invisible from the game until somebody notices their score is
// someone else's:
//   - two players who both type MIKE must get two rows, not one
//   - renaming yourself must keep your history, not abandon it
//   - a pre-D4 file must not be half-read into rows with invented identities
//
// Plus the bot prefix, which is what makes "humans only" a single character
// compare instead of a lookup.
//
//   g++ -std=c++17 -O2 -I server -I . server/test/scoreboard_test.cpp -o /tmp/sbt
//
// Run by server/test/run_all.sh.

#include "scoreboard.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (!ok) failures++;
}

static std::string TempPath(const char* leaf) {
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = (tmp && *tmp) ? tmp : "/tmp";
    if (!dir.empty() && dir.back() != '/') dir += '/';
    return dir + leaf;
}

static void WriteFile(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::trunc);
    f << body;
}

static std::string ReadFile(const std::string& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

int main() {
    std::cout << "scoreboard_test: the all-time table, keyed on identity\n";

    const std::string MIKE_A = "7f3a91c4aa0000000000000000000001";
    const std::string MIKE_B = "7f3a91c4aa0000000000000000000002";

    //MARK: The collision D4 exists to fix
    {
        Scoreboard sb;
        sb.addScore(MIKE_A, "MIKE", 10);
        sb.addScore(MIKE_B, "MIKE", 999);
        check(sb.scores.size() == 2,
              "two players both called MIKE get two rows (got " +
              std::to_string(sb.scores.size()) + ")");
        check(sb.scores[MIKE_A].score == 10 && sb.scores[MIKE_B].score == 999,
              "...and neither inherits the other's points");
    }

    //MARK: Renaming keeps your history
    {
        Scoreboard sb;
        sb.addScore(MIKE_A, "MIKE", 10);
        sb.addScore(MIKE_A, "MIKEY", 5);       // same person, new display name
        check(sb.scores.size() == 1, "renaming yourself does not start a new row");
        check(sb.scores[MIKE_A].score == 15, "...the total carries over");
        check(sb.scores[MIKE_A].name == "MIKEY",
              "...and the board shows the CURRENT name (" + sb.scores[MIKE_A].name + ")");
    }

    //MARK: Bots, and the prefix that sorts them apart
    {
        check(IsBotId(BotIdFor("DJEFF")), "a bot id reads as a bot");
        check(!IsBotId(MIKE_A), "a human id does not");
        check(BotIdFor("DJEFF") < MIKE_A,
              "'-' sorts below hex, so bots group ahead of every human");
        check(!IsBotId(""), "an empty id is not a bot (it is nothing)");

        Scoreboard sb;
        sb.addScore(MIKE_A, "MIKE", 10);
        sb.addScore(BotIdFor("DJEFF"), "DJEFF", 500);
        sb.addScore(BotIdFor("JURF"),  "JURF",  400);
        sb.generateLeaderboard();

        const auto humans = sb.top(/*bots*/ false);
        const auto bots   = sb.top(/*bots*/ true);
        check(humans.size() == 1 && humans[0].Name == "MIKE",
              "the human list holds only humans");
        check(bots.size() == 2 && bots[0].Name == "DJEFF",
              "the bot list holds only bots, ranked");
        // The whole reason to separate them: bots play every match, so on a
        // combined board they bury everyone.
        check(sb.leaderboard.size() == 3 && sb.leaderboard[0].IsBot,
              "on a combined board the bot outranks the human, which is why they split");
    }

    //MARK: An identity is required
    {
        Scoreboard sb;
        sb.addScore("", "NOBODY", 50);
        check(sb.scores.empty(),
              "a credit with no identity is dropped, not given an invented key");
    }

    //MARK: Round trip through the file
    {
        const std::string path = TempPath("platformz-sbt-roundtrip");
        std::remove(path.c_str());
        Scoreboard out;
        out.setFilePath(path);
        out.addScore(MIKE_A, "MIKE", 10);
        out.addScore(MIKE_B, "MIKE", 999);
        out.addScore(BotIdFor("DJEFF"), "DJEFF", 500);
        check(out.save(), "saved");

        Scoreboard in;
        in.setFilePath(path);
        check(in.load(), "loaded");
        check(in.scores.size() == 3, "all three rows survived");
        check(in.scores[MIKE_A].score == 10 && in.scores[MIKE_A].name == "MIKE",
              "a row keeps its score and its name");
        check(in.scores[MIKE_B].score == 999, "...and the two MIKEs stayed apart");
        check(IsBotId(BotIdFor("DJEFF")) && in.scores.count(BotIdFor("DJEFF")) == 1,
              "the bot row survived as a bot");
        std::remove(path.c_str());
    }

    //MARK: A name with spaces, which the format has to survive
    {
        const std::string path = TempPath("platformz-sbt-spaces");
        std::remove(path.c_str());
        Scoreboard out;
        out.setFilePath(path);
        out.addScore(BotIdFor("RoyBOT OVERLORD"), "RoyBOT OVERLORD", 7);
        out.save();
        Scoreboard in;
        in.setFilePath(path);
        in.load();
        check(in.scores.count(BotIdFor("RoyBOT OVERLORD")) == 1 &&
              in.scores[BotIdFor("RoyBOT OVERLORD")].name == "RoyBOT OVERLORD",
              "a name with spaces round-trips (everything past the 2nd tab is the name)");
        std::remove(path.c_str());
    }

    //MARK: The migration - a pre-D4 file
    {
        // The old format was "<score>\t<name>". Every line has two fields, so
        // every line is skipped: there is no way to know whose identity a
        // display name belonged to, and guessing is the exact bug being removed.
        const std::string path = TempPath("platformz-sbt-legacy");
        WriteFile(path, "10\tMIKE\n999\tMIKE\n500\tDJEFF\n");
        Scoreboard sb;
        sb.setFilePath(path);
        const bool ok = sb.load();
        check(!ok, "a pre-D4 file does not load");
        check(sb.scores.empty(), "...and leaves an empty table, not a half-read one");
        std::remove(path.c_str());
    }

    //MARK: Tolerance, which the old loader had and must keep
    {
        const std::string path = TempPath("platformz-sbt-damaged");
        WriteFile(path,
                  "C\t10\t1\t" + MIKE_A + "\tMIKE\n"
                  "\n"                                    // blank
                  "nonsense\n"                            // no tabs at all
                  "C\tabc\t1\t" + MIKE_B + "\tBAD SCORE\n"  // unreadable score
                  "C\t7\t1\t\tNO IDENTITY\n"             // empty id field
                  "R\t5\t1000\tMEDIUM\n"                  // a run, truncated
                  "X\t1\t2\t3\t4\n"                        // unknown line type
                  "C\t42\t3\t" + MIKE_B + "\tOTHER\n");
        Scoreboard sb;
        sb.setFilePath(path);
        check(sb.load(), "a damaged file still loads");
        check(sb.scores.size() == 2,
              "the good rows survive and the bad ones are skipped (got " +
              std::to_string(sb.scores.size()) + " of 2)");
        check(sb.scores[MIKE_B].score == 42, "the last good row wins for an id");
        std::remove(path.c_str());
    }

    //MARK: Save is atomic, and leaves no litter
    {
        const std::string path = TempPath("platformz-sbt-atomic");
        std::remove(path.c_str());
        Scoreboard sb;
        sb.setFilePath(path);
        sb.addScore(MIKE_A, "MIKE", 1);
        sb.save();
        check(ReadFile(path + ".tmp").empty(), "the temp file is gone after a save");
        check(!ReadFile(path).empty(), "and the real file is there");
        std::remove(path.c_str());
    }

    //MARK: Runs - the arcade board (D5)
    auto run = [](int score, const std::string& id, const std::string& name,
                  bool official = true, long long when = 1000) {
        RunRow r;
        r.score = score; r.id = id; r.name = name;
        r.official = official; r.when = when; r.map = "MEDIUM";
        r.matchName = official ? "OFFICIAL MATCH" : "MIKE'S MATCH";
        return r;
    };

    {
        Scoreboard sb;
        // One player, five good nights. The cap keeps three.
        for (int i = 1; i <= 5; ++i) sb.addRun(run(i * 10, MIKE_A, "MIKE"));
        check(sb.runs.size() == 3, "a player holds at most 3 rows (got " +
                                   std::to_string(sb.runs.size()) + ")");
        check(sb.runs[0].score == 50 && sb.runs[2].score == 30,
              "...and they are the best three, best first");
    }

    {
        // The same player CAN hold several slots - that is the point of an arcade
        // board, and what makes beating your own third place a normal evening.
        Scoreboard sb;
        sb.addRun(run(90, MIKE_A, "MIKE"));
        sb.addRun(run(80, MIKE_A, "MIKE"));
        sb.addRun(run(70, MIKE_B, "OTHER"));
        const auto top = sb.topRuns(false, 10);
        check(top.size() == 3 && top[0].id == MIKE_A && top[1].id == MIKE_A,
              "one player occupies more than one slot");
    }

    {
        // Bots share one identity and hold ONE row between them. Nine bot names at
        // three rows each would be 27 entries competing for a board of ten.
        Scoreboard sb;
        sb.addRun(run(500, RUN_BOT_ID, "GEOFF"));
        sb.addRun(run(400, RUN_BOT_ID, "JURF"));
        sb.addRun(run(300, RUN_BOT_ID, "DJEFF"));
        sb.addRun(run(10,  MIKE_A,     "MIKE"));
        const auto top = sb.topRuns(false, 10);
        int bots = 0;
        for (const RunRow& r : top) if (r.isBot()) bots++;
        check(bots == 1, "all the bots together hold one row (got " +
                         std::to_string(bots) + ")");
        check(top[0].name == "GEOFF",
              "...displaying the bot that actually set it, not the shared id");
    }

    {
        // Official-only filters on READ. The custom runs are still there, so
        // turning it back off restores them - which is the whole reason `official`
        // is recorded before the switch exists.
        Scoreboard sb;
        sb.addRun(run(900, MIKE_A, "MIKE", /*official*/ false));
        sb.addRun(run(100, MIKE_B, "OTHER", /*official*/ true));
        check(sb.topRuns(false, 10).size() == 2, "unfiltered shows both");
        const auto official = sb.topRuns(true, 10);
        check(official.size() == 1 && official[0].score == 100,
              "official-only shows just the official run");
        check(sb.runs.size() == 2, "...and the custom run is still ON DISK, not dropped");
    }

    {
        // The cap is PER KIND. Trim to a global top-N instead and a run of
        // high-scoring custom matches evicts the official rows, so turning the
        // switch on months later reveals a near-empty board.
        Scoreboard sb;
        for (int i = 1; i <= 5; ++i) sb.addRun(run(900 + i, MIKE_A, "MIKE", false));
        for (int i = 1; i <= 5; ++i) sb.addRun(run(i, MIKE_A, "MIKE", true));
        const auto official = sb.topRuns(true, 10);
        check(official.size() == 3,
              "the low-scoring OFFICIAL runs survive a flood of better custom ones (got " +
              std::to_string(official.size()) + ")");
    }

    {
        // The pin, and the reason the trim is per-identity: a player outside the
        // top ten still has to see their own best.
        Scoreboard sb;
        for (int i = 0; i < 12; ++i)
            sb.addRun(run(1000 + i, "aa" + std::to_string(i) + std::string(30, 'f'), "SOMEBODY"));
        sb.addRun(run(5, MIKE_A, "MIKE"));
        const auto top = sb.topRuns(false, 10);
        check(top.size() == 10, "the board is ten rows");
        bool onBoard = false;
        for (const RunRow& r : top) if (r.id == MIKE_A) onBoard = true;
        check(!onBoard, "our low scorer is nowhere near it");
        RunRow best;
        check(sb.bestRunFor(MIKE_A, false, best) && best.score == 5,
              "...but their best run survived the trim, so it can still be pinned");
        RunRow none;
        check(!sb.bestRunFor("never-played", false, none),
              "a player with no runs has nothing to pin");
    }

    {
        // A run's name is FROZEN; a career row's follows renames. Opposite rules
        // on purpose - a run is a historical event, a total belongs to a person.
        Scoreboard sb;
        sb.addRun(run(10, MIKE_A, "MIKE"));
        sb.addScore(MIKE_A, "MIKE", 10);
        sb.addRun(run(20, MIKE_A, "MIKEY"));
        sb.addScore(MIKE_A, "MIKEY", 20);
        check(sb.runs.size() == 2, "two runs");
        check(sb.runs[0].name == "MIKEY" && sb.runs[1].name == "MIKE",
              "each run keeps the name it was set under");
        check(sb.scores[MIKE_A].name == "MIKEY",
              "...while the career row shows the current one");
        check(sb.scores[MIKE_A].matches == 2, "and counts matches (for a future average)");
    }

    {
        // Both tables through one file, one rename.
        const std::string path = TempPath("platformz-sbt-d5");
        std::remove(path.c_str());
        Scoreboard out;
        out.setFilePath(path);
        out.addScore(MIKE_A, "MIKE", 42);
        out.addRun(run(99, MIKE_A, "MIKE", true, 1700000000LL));
        out.addRun(run(50, RUN_BOT_ID, "RoyBOT OVERLORD", false));
        check(out.save(), "saved both tables");

        Scoreboard in;
        in.setFilePath(path);
        check(in.load(), "loaded");
        check(in.scores.size() == 1 && in.scores[MIKE_A].score == 42 &&
              in.scores[MIKE_A].matches == 1, "the career row round-tripped with its count");
        check(in.runs.size() == 2, "both runs round-tripped");
        const RunRow& top = in.runs[0];
        check(top.score == 99 && top.when == 1700000000LL && top.map == "MEDIUM" &&
              top.official && top.matchName == "OFFICIAL MATCH" && top.name == "MIKE",
              "every field survived, including the timestamp nothing displays yet");
        // Two free-text fields in one line, which only works because both are
        // clampName'd and so cannot contain a tab.
        check(in.runs[1].matchName == "MIKE'S MATCH" &&
              in.runs[1].name == "RoyBOT OVERLORD",
              "a room name and a display name with spaces both survive");
        std::remove(path.c_str());
    }

    {
        // A D4 file is skipped wholesale, exactly as a pre-D4 one was: the line
        // has no type column, and there is no run history to invent for it.
        const std::string path = TempPath("platformz-sbt-d4file");
        WriteFile(path, "10\t" + MIKE_A + "\tMIKE\n999\t" + MIKE_B + "\tOTHER\n");
        Scoreboard sb;
        sb.setFilePath(path);
        check(!sb.load(), "a D4-format file does not load");
        check(sb.scores.empty() && sb.runs.empty(), "...and leaves both tables empty");
        std::remove(path.c_str());
    }

    if (failures) { std::cout << failures << " CHECK(S) FAILED\n"; return 1; }
    std::cout << "scoreboard_test: all checks passed\n";
    return 0;
}
