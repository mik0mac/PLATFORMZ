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

//MARK: A row
struct ScoreRow {
    int         score = 0;
    std::string name;   // as of the last credit - display only, never a key
};

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

    std::string filePath;  // "<score>\t<id>\t<name>", one line per row
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
        if (!name.empty()) row.name = name;
    }
    void removeScore(const std::string& id) { scores.erase(id); }
    void clearScores() {
        scores.clear();
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
        std::string line;
        int legacy = 0;
        while (std::getline(file, line)) {
            if (line.empty()) continue;

            const size_t tab1 = line.find('\t');
            if (tab1 == std::string::npos) {
                std::cerr << "[scoreboard] skipping malformed line: " << line << "\n";
                continue;
            }
            const size_t tab2 = line.find('\t', tab1 + 1);
            if (tab2 == std::string::npos) {
                // Two fields: a pre-D4 "<score>\t<name>" row, from before the
                // table was keyed on identity. Counted and reported once rather
                // than logged per line, so a 100-row legacy file says one clear
                // thing instead of scrolling the boot log.
                legacy++;
                continue;
            }

            int score = 0;
            try {
                score = std::stoi(line.substr(0, tab1));
            } catch (const std::exception&) {
                // invalid_argument (not a number) or out_of_range (too big for int).
                std::cerr << "[scoreboard] skipping line with an unreadable score: " << line << "\n";
                continue;
            }

            const std::string id = line.substr(tab1 + 1, tab2 - tab1 - 1);
            if (id.empty()) {
                std::cerr << "[scoreboard] skipping line with no identity: " << line << "\n";
                continue;
            }
            // Score, then id, then name: the name is the only field that can hold
            // arbitrary text, so everything past the SECOND tab is the name and
            // nothing needs escaping. (clampName strips control characters, tab
            // included, so a name can never split a line.)
            ScoreRow row;
            row.score = score;
            row.name  = line.substr(tab2 + 1);
            parsed[id] = row;
        }
        file.close();

        if (legacy > 0) {
            std::cout << "[scoreboard] ignored " << legacy << " pre-D4 line(s) from "
                      << filePath << " - that table was keyed on display name and"
                      << " cannot be mapped onto identities. Starting clean;"
                      << " keep a copy of the old file if you want the numbers.\n";
        }
        if (parsed.empty()) {
            std::cerr << "[scoreboard] " << filePath << " held no readable scores"
                      << " - keeping the current table\n";
            return false;
        }

        scores = std::move(parsed);
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

            // One "<score>\t<id>\t<name>" line per row, streamed straight out
            // rather than accumulated - no reason to build the whole file in
            // memory first.
            for (const auto& [id, row] : scores) {
                file << row.score << '\t' << id << '\t' << row.name << '\n';
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
