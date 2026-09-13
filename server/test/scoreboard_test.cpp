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
                  "10\t" + MIKE_A + "\tMIKE\n"
                  "\n"                                   // blank
                  "nonsense\n"                           // no tabs at all
                  "abc\t" + MIKE_B + "\tBAD SCORE\n"     // unreadable score
                  "7\t\tNO IDENTITY\n"                   // empty id field
                  "42\t" + MIKE_B + "\tOTHER\n");
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

    if (failures) { std::cout << failures << " CHECK(S) FAILED\n"; return 1; }
    std::cout << "scoreboard_test: all checks passed\n";
    return 0;
}
