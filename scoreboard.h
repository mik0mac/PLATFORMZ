#pragma once

#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstdio>    // std::rename, std::remove (atomic save)
#include <cstring>   // std::strerror
#include <cerrno>    // errno
#include <stdexcept> // std::invalid_argument / std::out_of_range from std::stoi

//MARK: What a row is keyed on
// The all-time table is keyed on the server-issued IDENTITY (D3), not on the
// display name. Names are neither unique nor owned, and keying on one meant:
// two players who both typed MIKE shared a row, anyone could claim someone
// else's by typing their name, and renaming yourself abandoned your history.
//
// So the name is a PROPERTY of the row now. Rename yourself and the board
// follows you, because the row was never about the name.
//
//MARK: Bots are on the board too, and are told apart by their id
// A bot has no identity to be issued - nothing signs for it - so its row is
// keyed on its display name behind a '-'.
//
// The prefix is doing real work. A human id is 32 hex characters, so it can
// only ever start with [0-9a-f]; '-' is 0x2D, below '0' at 0x30. That makes the
// two classes DISJOINT BY CONSTRUCTION, makes "is this a bot" a single character
// compare rather than a lookup or a parallel flag, and makes bots sort ahead of
// every human for free in the std::map and in the file - the same ergonomics a
// negative number would give, without turning a 128-bit id into an integer.
const char BOT_ID_PREFIX = '-';

inline bool IsBotId(const std::string& id) {
    return !id.empty() && id[0] == BOT_ID_PREFIX;
}
inline std::string BotIdFor(const std::string& botName) {
    return std::string(1, BOT_ID_PREFIX) + botName;
}

//MARK: The RUN board's bot identity
// On the CAREER table each bot keeps its own row (`-GEOFF`, `-JURF`, ...). On the
// RUN board they all share this one, because 9 bot names at 3 rows each would be
// 27 entries competing for a board of 10 and would reserve no space at all for
// humans. Shared, and capped at a single row, the bots contribute one line:
// the best a bot has ever managed, which is a better thing to chase than ten.
//
// The row still DISPLAYS the bot that set it - `name` is stored apart from the
// id precisely so those two can differ. Do not "tidy up" that duplication.
inline const char* RUN_BOT_ID = "-BOT";

//MARK: A row
struct ScoreRow {
    int         score   = 0;
    std::string name;   // as of the last credit - display only, never a key
    // Matches this identity has been credited for. The one int that lets a
    // career board be ranked by AVERAGE rather than by volume; without it the
    // board permanently rewards whoever played most and no newcomer can catch
    // up. Counted from day one even though nothing displays it yet - it cannot
    // be reconstructed later from rows that never had it.
    int         matches = 0;
};

//MARK: One run
// A finished match, from one player's side. Unlike a career row this is a
// historical EVENT, which is why `name` is frozen at record time: renaming
// yourself does not rewrite what happened that Tuesday. The career row does the
// opposite and follows renames, because a total belongs to a person. That
// asymmetry is deliberate - see docs/matchmaking-plan.md D5.
struct RunRow {
    int         score = 0;
    long long   when  = 0;      // Unix seconds. WALL clock, unlike everything
                                // else in the server - see the note on save().
    std::string map;            // SMALL|MEDIUM|LARGE|XL. Stored, not displayed yet.
    bool        official = false; // server-assigned truth, NOT derived from the
                                  // room's name, which a player chooses and could
                                  // set to "OFFICIAL MATCH"
    std::string matchName;      // the room's display name
    std::string id;             // identity (or RUN_BOT_ID)
    std::string name;           // display name AS IT WAS, frozen

    bool isBot() const { return IsBotId(id); }

    // Best first, with a stable tiebreak so the order does not wobble between
    // runs of the same data.
    bool operator<(const RunRow& o) const {
        if (score != o.score) return score > o.score;
        if (when  != o.when)  return when  > o.when;   // newer first
        return id < o.id;
    }
};

// How many rows one identity may hold on the board, per kind. A human gets three
// so "beat your own third place" is a real thing; bots get one between them.
inline int RunRowsAllowed(const std::string& id) { return IsBotId(id) ? 1 : 3; }


struct rankingByScore
{
    int Score;
    std::string Name;
    std::string Id;      // so a consumer can tell a bot from a human, and tell
                         // two players with the same name apart
    bool IsBot = false;

    // Deliberately inverted: "less than" means "higher score", so anything that
    // sorts ascending by this rule (std::partial_sort below) comes out ranked
    // best-first. Id breaks ties so the order is stable between runs - the NAME
    // cannot do that any more, because two rows are allowed to share one.
    bool operator<(const rankingByScore& other) const {
        if (Score != other.Score) {
            return Score > other.Score;
        }
        return Id < other.Id;
    }
};



class Scoreboard
{
public:
    // identity -> row. See the note above for what an identity is and why a bot's
    // starts with '-'.
    std::map<std::string, ScoreRow> scores;
    std::vector<rankingByScore> leaderboard;   // ranked, best first, humans AND bots

    std::string filePath;  // "C|R <fields...>", one line per row - see save()
    size_t defaultCount = 10; // rows of each class to publish

    // Rank the whole table. Bots are included and tagged rather than filtered
    // here: which class a caller wants is the caller's business (the wire packs
    // the top few of each, the client shows one at a time), and a ranking that
    // silently dropped half its input would be a surprising thing to reuse.
    void generateLeaderboard(size_t = 0) {
        // reserve() only sets capacity - it drops nothing. Without the clear() a
        // second call appends the whole map on top of the first batch, and the
        // leaderboard lists every player twice.
        leaderboard.clear();
        leaderboard.reserve(scores.size());
        for (const auto& [id, row] : scores) {
            rankingByScore rbs;
            rbs.Score = row.score;
            rbs.Name  = row.name.empty() ? id : row.name;
            rbs.Id    = id;
            rbs.IsBot = IsBotId(id);
            leaderboard.push_back(rbs);
        }
        std::sort(leaderboard.begin(), leaderboard.end());
    }

    // The top `count` rows of one class. The whole reason bots carry a '-' id.
    std::vector<rankingByScore> top(bool bots, size_t count = 0) const {
        if (count == 0) count = defaultCount;
        std::vector<rankingByScore> out;
        for (const rankingByScore& r : leaderboard) {
            if (r.IsBot != bots) continue;
            out.push_back(r);
            if (out.size() >= count) break;
        }
        return out;
    }

    // Add to this identity's running total, refreshing the display name it
    // carries. The name is refreshed on EVERY credit on purpose: that is what
    // makes renaming yourself keep your history and show your current name.
    void addScore(const std::string& id, const std::string& name, int score) {
        if (id.empty()) return;      // no identity, no row - see the server's credit path
        ScoreRow& row = scores[id];
        row.score += score;
        row.matches++;
        if (!name.empty()) row.name = name;
    }

    //MARK: Runs
    std::vector<RunRow> runs;

    // Record one finished match from one player's side, then re-apply the cap.
    void addRun(RunRow r) {
        if (r.id.empty()) return;
        runs.push_back(std::move(r));
        trimRuns();
    }

    // Keep, per kind, each identity's best few - three for a human, one for the
    // bots between them.
    //
    // PER IDENTITY rather than a flat "top N of the whole board", for two
    // reasons. The one the design called out: trim to a global top N and a run of
    // high-scoring CUSTOM matches evicts the official rows, so turning on
    // official-only months later reveals a near-empty board with the evidence
    // long gone. The one that only appeared on writing it: the board pins a
    // player's personal best under the top ten, and a global trim throws away the
    // personal best of everybody outside it - which is most people, which is who
    // the pin is for.
    //
    // The file therefore grows with DISTINCT PLAYERS (at most 3 rows each per
    // kind), not with matches played. The career table already grows the same way
    // for the same reason.
    void trimRuns() {
        std::sort(runs.begin(), runs.end());       // best first
        std::map<std::pair<bool, std::string>, int> kept;   // (official, id) -> count
        std::vector<RunRow> out;
        out.reserve(runs.size());
        for (RunRow& r : runs) {
            int& n = kept[{r.official, r.id}];
            if (n >= RunRowsAllowed(r.id)) continue;
            n++;
            out.push_back(std::move(r));
        }
        runs = std::move(out);
    }

    // The board: best `count` runs, best first.
    //
    // `officialOnly` filters on READ, never on record - which is what makes the
    // switch free in both directions and retroactive on rows already on disk. Flip
    // it back and the custom runs are all still there.
    std::vector<RunRow> topRuns(bool officialOnly, size_t count) const {
        std::vector<RunRow> out;
        for (const RunRow& r : runs) {
            if (officialOnly && !r.official) continue;
            out.push_back(r);
            if (out.size() >= count) break;
        }
        return out;
    }

    // This identity's best run, for the row pinned under the board. Returns false
    // if they have never recorded one.
    bool bestRunFor(const std::string& id, bool officialOnly, RunRow& out) const {
        if (id.empty()) return false;
        for (const RunRow& r : runs) {          // already sorted best first
            if (r.id != id) continue;
            if (officialOnly && !r.official) continue;
            out = r;
            return true;
        }
        return false;
    }
    void removeScore(const std::string& id) { scores.erase(id); }
    void clearScores() {
        scores.clear();
        runs.clear();
        leaderboard.clear();
    }

    // MARK: File, load, save
    void setFilePath(const std::string& path) {
        filePath = path;
    }

    // Persistence is best-effort: a missing or unreadable file must never bring the
    // process down, so every failure path logs and leaves the in-memory table as it
    // was. Returns true only when the file was read and parsed in full.
    bool load() {
        if (filePath.empty()) {
            std::cerr << "[scoreboard] no file path set - nothing to load\n";
            return false;
        }

        std::ifstream file(filePath);
        if (!file.is_open()) {
            // Not an error: on a cold start the file simply doesn't exist yet.
            // The first save() creates it.
            std::cout << "[scoreboard] no file at " << filePath << " - starting empty\n";
            return false;
        }

        // Parse into a local and commit only once it has fully succeeded. Reading
        // straight into scores (or clearing first) would leave an empty or
        // half-loaded table behind if the file turned out to be truncated or the
        // wrong shape.
        // Read line by line, each one "<score>\t<id>\t<name>". A damaged line is
        // skipped rather than abandoning the load - that resilience is the main
        // reason this format beats JSON here, where one bad byte costs the whole
        // file. It is also how the D4 format change migrates: every line of the
        // old "<score>\t<name>" file has two fields, so it is skipped, and the
        // board starts clean rather than inventing identities for rows whose
        // owners cannot be known.
        std::map<std::string, ScoreRow> parsed;
        std::vector<RunRow> parsedRuns;
        std::string line;
        int legacy = 0;
        while (std::getline(file, line)) {
            if (line.empty()) continue;

            // Split on tabs. Safe as a fixed-field format ONLY because every text
            // field is written through clampName, which strips control characters
            // including tab - so neither a player's name nor a room's can split a
            // line. A future field that skips that filter breaks parsing here in a
            // way that will look like file corruption.
            std::vector<std::string> f;
            size_t start = 0;
            for (;;) {
                const size_t tab = line.find('\t', start);
                if (tab == std::string::npos) { f.push_back(line.substr(start)); break; }
                f.push_back(line.substr(start, tab - start));
                start = tab + 1;
            }

            // A line-type column tells the two tables apart. Anything else is
            // from before D5 - including D4's "<score>\t<id>\t<name>" and the
            // pre-D4 "<score>\t<name>" - and is skipped, which IS the migration.
            // Counted and reported once rather than logged per line, so an old
            // file says one clear thing instead of scrolling the boot log.
            if (f.size() < 2 || (f[0] != "C" && f[0] != "R")) { legacy++; continue; }

            auto num = [&](const std::string& v, long long& out) {
                try { out = std::stoll(v); return true; }
                catch (const std::exception&) {
                    std::cerr << "[scoreboard] skipping line with an unreadable number: "
                              << line << "\n";
                    return false;
                }
            };

            if (f[0] == "C") {
                // C <score> <matches> <id> <name>
                if (f.size() < 5) { legacy++; continue; }
                long long score = 0, matches = 0;
                if (!num(f[1], score) || !num(f[2], matches)) continue;
                if (f[3].empty()) {
                    std::cerr << "[scoreboard] skipping career line with no identity: "
                              << line << "\n";
                    continue;
                }
                ScoreRow row;
                row.score   = (int)score;
                row.matches = (int)matches;
                row.name    = f[4];
                parsed[f[3]] = row;
            } else {
                // R <score> <when> <map> <kind> <matchName> <id> <name>
                if (f.size() < 8) { legacy++; continue; }
                long long score = 0, when = 0;
                if (!num(f[1], score) || !num(f[2], when)) continue;
                if (f[6].empty()) {
                    std::cerr << "[scoreboard] skipping run line with no identity: "
                              << line << "\n";
                    continue;
                }
                RunRow r;
                r.score     = (int)score;
                r.when      = when;
                r.map       = f[3];
                r.official  = (f[4] == "official");
                r.matchName = f[5];
                r.id        = f[6];
                r.name      = f[7];
                parsedRuns.push_back(std::move(r));
            }
        }
        file.close();

        if (legacy > 0) {
            std::cout << "[scoreboard] ignored " << legacy << " line(s) from "
                      << filePath << " in an older format - nothing is lost that"
                      << " could have been carried over. Starting clean.\n";
        }
        if (parsed.empty() && parsedRuns.empty()) {
            std::cerr << "[scoreboard] " << filePath << " held no readable scores"
                      << " - keeping the current table\n";
            return false;
        }

        scores = std::move(parsed);
        runs   = std::move(parsedRuns);
        trimRuns();            // also sorts, which everything downstream assumes
        generateLeaderboard();
        return true;
    }

    // Writes to a temporary, then renames it over the real file. rename() is atomic
    // on POSIX, so an interrupted save can never leave a half-written or empty file
    // for the next load() to choke on - the file on disk is always either the
    // complete old version or the complete new one.
    bool save() const {
        if (filePath.empty()) {
            std::cerr << "[scoreboard] no file path set - nothing to save\n";
            return false;
        }

        const std::string tmpPath = filePath + ".tmp";
        {
            std::ofstream file(tmpPath, std::ios::trunc);
            if (!file.is_open()) {
                std::cerr << "[scoreboard] cannot open " << tmpPath << " for writing: "
                          << std::strerror(errno) << "\n";
                return false;
            }

            // Both tables, one file, a line-type column telling them apart.
            //
            // ONE file because a match end writes both a run row and a career
            // credit: two files would be two saves, and a crash between them
            // leaves a run recorded that no career total reflects. One rename()
            // is both or neither.
            //
            // Streamed straight out rather than accumulated - no reason to build
            // the whole file in memory first.
            for (const auto& [id, row] : scores) {
                file << "C\t" << row.score << '\t' << row.matches << '\t'
                     << id << '\t' << row.name << '\n';
            }
            // `when` is WALL time (Unix seconds), the only wall clock in the
            // server - everything else is the steady clock, whose zero is
            // arbitrary and cannot be turned into a date at all. Safe here
            // because it is display-only: a stepped clock puts a wrong date on a
            // leaderboard row, where the same mistake in the reap timer would
            // tear down live matches.
            for (const RunRow& r : runs) {
                file << "R\t" << r.score << '\t' << r.when << '\t' << r.map << '\t'
                     << (r.official ? "official" : "custom") << '\t'
                     << r.matchName << '\t' << r.id << '\t' << r.name << '\n';
            }

            // is_open() only proved the file opened. A full disk fails at write time,
            // and without this check we would rename a truncated file over a good one.
            file.close();
            if (!file) {
                std::cerr << "[scoreboard] write to " << tmpPath << " failed: "
                          << std::strerror(errno) << "\n";
                std::remove(tmpPath.c_str());
                return false;
            }
        }

        if (std::rename(tmpPath.c_str(), filePath.c_str()) != 0) {
            std::cerr << "[scoreboard] could not replace " << filePath << ": "
                      << std::strerror(errno) << "\n";
            std::remove(tmpPath.c_str());
            return false;
        }
        return true;
    }
};
