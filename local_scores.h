// local_scores.h
//
// The LOCAL high-score board: the same arcade table as the server's, kept on
// this machine, fed only by LOCAL matches, and never mixed with the online one.
//
//MARK: Why it is a separate board rather than the same one offline
// The online board is a shared, server-refereed record. Nothing about a local
// match is refereed - the client hosts its own sim, so the score is whatever the
// client says it is, and the options are whatever the player set them to. Five
// bots on EASY in an XL arena is not the same game as an official room, and
// letting the two share a table would either corrupt the shared one or force the
// local one to pretend it was earned. They are two different questions:
//
//   online   "how do I compare to everyone?"
//   local    "how do I compare to my own best evening, and to the bots?"
//
// So: two boards, same rules (runboard.h), same modal, one tab apart.
//
//MARK: What the player and the bots are keyed on
// The bots are keyed exactly as they are online - all of them behind RUN_BOT_ID,
// so they hold one row between them and the "factory high score" is a single
// line to knock off. That matters MORE here than online: a local match has
// exactly one human in it, so bots with a row each would own the whole board.
//
// The human is keyed on LOCAL_PLAYER_ID, a constant - deliberately NOT the
// profile's clientId. There is one person at this keyboard, and coupling the
// board to the profile's id means a corrupt or deleted profile mints a fresh
// clientId and every run already on disk silently becomes somebody else's: the
// rows would still be listed, but YOUR BEST would go blank and no new score
// could ever match them. A constant cannot do that. Renaming yourself is
// already handled the way it is online - `name` is frozen per row, so old runs
// keep the name they were set under.
//
//MARK: Storage
// Through profile.h, which is the client's only persistence layer and already
// solves the two hard parts on both platforms: write-then-rename so a crash
// cannot truncate the file, and localStorage (with its exceptions caught) on the
// web. Its own FILE though, not a key inside profile.json - clearing your scores
// should not be able to cost you your name, your identity token, or your saved
// rules, and a parse failure in one should not take the other down with it.
//
// It never needs a size cap. TrimRuns bounds the table at three rows for the
// player and one for the bots, per official-flag - four lines, forever.

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "runboard.h"
#include "profile.h"

namespace localscores {

//MARK: Format version
// Bumped only if a future build has to MIGRATE rather than ignore what it does
// not recognise. Additive changes need no bump: every read below has a default.
constexpr int FORMAT_VERSION = 1;

// Rows shown on the board. Ten, matching the server's SCORES_BOARD_ROWS - the
// modal is the same modal, and a board that changed height when you switched
// tabs would look like a bug.
constexpr size_t BOARD_ROWS = 10;

// The one human. See the header note: a constant, not the clientId.
inline const char* LOCAL_PLAYER_ID = "you";

//MARK: The board
struct Board {
    std::vector<RunRow> runs;   // sorted best-first and trimmed by TrimRuns

    // Record one finished LOCAL match from one player's side, then re-apply the
    // cap. Mirrors the server's credit path exactly, including the bot sharing.
    void addRun(RunRow r) {
        if (r.id.empty()) return;
        runs.push_back(std::move(r));
        TrimRuns(runs);
    }

    // Local runs are never official - there is no referee - so both of these
    // pass officialOnly=false. The parameter still exists on the shared rules
    // because the SERVER's board needs it; passing it here would be pretending
    // the distinction means something locally.
    std::vector<RunRow> top(size_t count = BOARD_ROWS) const {
        return TopRuns(runs, /*officialOnly=*/false, count);
    }
    bool bestForPlayer(RunRow& out) const {
        return BestRunFor(runs, LOCAL_PLAYER_ID, /*officialOnly=*/false, out);
    }

    // Where the run recorded at `when` for `id` sits on the board, 1-based, or 0
    // if it is not on it. Asked right after a match so the game-over screen can
    // say so, which is the only moment a high-score table is actually exciting.
    //
    // Answered by LOOKING at the trimmed board rather than by counting better
    // scores, because those two disagree exactly where it matters. Count better
    // scores and a fourth personal best reads as "#4" - it beat nothing on the
    // board, but three of the rows above it are the player's own, so TrimRuns
    // throws it away the moment it is added. Looking says 0, which is the truth.
    int rankOf(const std::string& id, long long when) const {
        const std::vector<RunRow> board = top();
        for (size_t i = 0; i < board.size(); ++i)
            if (board[i].id == id && board[i].when == when) return (int)i + 1;
        return 0;
    }

    void clear() { runs.clear(); }
};

inline Board& Get() { static Board b; return b; }

//MARK: Serialize
// Field by field with a default for every read, for the same reason profile.h
// does it: this is a STORAGE format that a build written a year from now still
// has to open, not the wire format, which is free to change whenever both ends
// ship together.
inline std::string Serialize(const Board& b) {
    nlohmann::json rows = nlohmann::json::array();
    for (const RunRow& r : b.runs) {
        rows.push_back({
            {"s",    r.score},
            {"w",    r.when},
            {"map",  r.map},
            {"m",    r.matchName},
            {"id",   r.id},
            {"n",    r.name},
        });
    }
    // `official` is not written: every local run is false by construction, and a
    // field that can only hold one value is a field that will eventually hold
    // the wrong one. The loader sets it explicitly instead.
    nlohmann::json j = {{"version", FORMAT_VERSION}, {"runs", rows}};
    return j.dump();
}

// Returns false when there was nothing usable - which is the normal first-launch
// case, not an error. A damaged ROW is skipped and the rest of the file still
// loads, the same tolerance the server's line-based file has: one bad byte
// should cost one score, not the whole history.
inline bool Deserialize(const std::string& raw, Board& b) {
    if (raw.empty()) return false;
    nlohmann::json j = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    auto it = j.find("runs");
    if (it == j.end() || !it->is_array()) return false;

    std::vector<RunRow> parsed;
    for (const nlohmann::json& o : *it) {
        if (!o.is_object()) continue;
        // Type-checked, not just presence-checked: this is a file the player can
        // open in a text editor, and "s": "lots" must not throw.
        auto num = [&](const char* k, long long& dst) {
            auto f = o.find(k);
            if (f == o.end() || !f->is_number_integer()) return false;
            dst = f->get<long long>();
            return true;
        };
        auto str = [&](const char* k, std::string& dst) {
            auto f = o.find(k);
            if (f != o.end() && f->is_string()) dst = f->get<std::string>();
        };
        long long score = 0, when = 0;
        if (!num("s", score)) continue;       // a row with no score is not a row
        num("w", when);
        RunRow r;
        r.score    = (int)score;
        r.when     = when;
        r.official = false;                   // see Serialize
        str("map", r.map);
        str("m",   r.matchName);
        str("id",  r.id);
        str("n",   r.name);
        if (r.id.empty()) continue;           // no key, no row - same rule as the server
        parsed.push_back(std::move(r));
    }
    if (parsed.empty()) return false;
    b.runs = std::move(parsed);
    TrimRuns(b.runs);        // also sorts, which everything downstream assumes
    return true;
}

//MARK: Load / save
// Same shape as profile::Load/Save, including the "skip a no-op write" cache:
// the board changes once per match, so without it the autosave would rewrite an
// identical blob every couple of seconds - which on the web is a synchronous
// localStorage write on the frame thread.
inline const char* STORAGE_LEAF = "scores.json";
inline const char* STORAGE_KEY  = "platformz.scores";

inline std::string& LastWritten() { static std::string s; return s; }

inline void Load() {
    Board& b = Get();
    b.clear();
    const std::string raw = profile::ReadRawFrom(STORAGE_LEAF, STORAGE_KEY);
    LastWritten() = Deserialize(raw, b) ? raw : std::string();
}

inline bool Save() {
    const std::string out = Serialize(Get());
    if (out == LastWritten()) return false;
    if (!profile::WriteRawTo(STORAGE_LEAF, STORAGE_KEY, out)) return false;
    LastWritten() = out;
    return true;
}

} // namespace localscores
