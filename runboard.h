// runboard.h
//
// What an arcade high-score board IS, with no opinion about where it is stored.
//
// There are two of these boards now - the server's, persisted to a file on the
// box and shared by everyone, and the client's LOCAL one, persisted beside the
// profile and private to this install - and they have to behave identically.
// A player should not have to learn a second set of rules for the same table
// just because one of them happens to be offline.
//
// So the rules live here, once, as free functions over a vector of rows:
//   - a run is a historical event, ranked best-first
//   - a human holds at most three rows, the bots one between them
//   - a board is the top N of those rows, filtered on READ
//   - and a player's own best is pinnable underneath
//
// `scoreboard.h` (server) and `local_scores.h` (client) both build on this and
// add only their own persistence. This header deliberately includes NOTHING that
// touches a filesystem - the web client compiles it, and there is no filesystem
// there worth the name.

#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

//MARK: Bots are on the board too, and are told apart by their id
// A bot has no identity to be issued - nothing signs for it - so its row is
// keyed on a name behind a '-'.
//
// The prefix is doing real work. A human id is 32 hex characters, so it can
// only ever start with [0-9a-f]; '-' is 0x2D, below '0' at 0x30. That makes the
// two classes DISJOINT BY CONSTRUCTION, makes "is this a bot" a single character
// compare rather than a lookup or a parallel flag, and makes bots sort ahead of
// every human for free in a std::map and in a file - the same ergonomics a
// negative number would give, without turning a 128-bit id into an integer.
const char BOT_ID_PREFIX = '-';

inline bool IsBotId(const std::string& id) {
    return !id.empty() && id[0] == BOT_ID_PREFIX;
}
inline std::string BotIdFor(const std::string& botName) {
    return std::string(1, BOT_ID_PREFIX) + botName;
}

//MARK: The RUN board's bot identity
// On the server's CAREER table each bot keeps its own row (`-GEOFF`, `-JURF`,
// ...). On a RUN board they all share this one, because 9 bot names at 3 rows
// each would be 27 entries competing for a board of 10 and would reserve no
// space at all for humans. Shared, and capped at a single row, the bots
// contribute one line: the best a bot has ever managed, which is a better thing
// to chase than ten.
//
// It matters more on the LOCAL board than on the server's, not less: there is
// exactly one human there to be crowded out.
//
// The row still DISPLAYS the bot that set it - `name` is stored apart from the
// id precisely so those two can differ. Do not "tidy up" that duplication.
inline const char* RUN_BOT_ID = "-BOT";

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

//MARK: The rules
// Keep, per kind, each identity's best few - three for a human, one for the bots
// between them. Sorts as a side effect, which everything downstream assumes.
//
// PER IDENTITY rather than a flat "top N of the whole board", for two reasons.
// The one the design called out: trim to a global top N and a run of
// high-scoring CUSTOM matches evicts the official rows, so turning on
// official-only months later reveals a near-empty board with the evidence long
// gone. The one that only appeared on writing it: the board pins a player's
// personal best under the top ten, and a global trim throws away the personal
// best of everybody outside it - which is most people, which is who the pin is
// for.
//
// A board therefore grows with DISTINCT PLAYERS (at most 3 rows each per kind),
// not with matches played. That bound is what lets the local board sit in the
// same storage drawer as the profile without ever needing a size cap: one
// player and one shared bot row is at most four lines, forever.
inline void TrimRuns(std::vector<RunRow>& runs) {
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

// The board: best `count` runs, best first. Assumes `runs` is already sorted,
// which TrimRuns guarantees.
//
// `officialOnly` filters on READ, never on record - which is what makes the
// switch free in both directions and retroactive on rows already on disk. Flip
// it back and the custom runs are all still there.
inline std::vector<RunRow> TopRuns(const std::vector<RunRow>& runs,
                                   bool officialOnly, size_t count) {
    std::vector<RunRow> out;
    for (const RunRow& r : runs) {
        if (officialOnly && !r.official) continue;
        out.push_back(r);
        if (out.size() >= count) break;
    }
    return out;
}

// This identity's best run, for the row pinned under the board. Returns false if
// they have never recorded one.
inline bool BestRunFor(const std::vector<RunRow>& runs, const std::string& id,
                       bool officialOnly, RunRow& out) {
    if (id.empty()) return false;
    for (const RunRow& r : runs) {          // already sorted best first
        if (r.id != id) continue;
        if (officialOnly && !r.official) continue;
        out = r;
        return true;
    }
    return false;
}

// Is this exact row one of the ones on `board`? The question both boards ask
// before pinning a personal best, so the pin is not a duplicate of a row three
// lines above it. Compared on the triple that identifies a RUN rather than on
// the whole struct: name is frozen per row and would match anyway, but map and
// matchName are free text and a == on those is a wider promise than is needed.
inline bool RunIsOnBoard(const std::vector<RunRow>& board, const RunRow& r) {
    for (const RunRow& b : board)
        if (b.id == r.id && b.score == r.score && b.when == r.when) return true;
    return false;
}
