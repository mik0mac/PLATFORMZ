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

struct rankingByScore
{
    int Score;
    std::string Name;

    // Deliberately inverted: "less than" means "higher score", so anything that
    // sorts ascending by this rule (std::partial_sort below) comes out ranked
    // best-first. Name breaks ties so the order is stable between runs.
    bool operator<(const rankingByScore& other) const {
        if (Score != other.Score) {
            return Score > other.Score;
        }
        return Name < other.Name;
    }
};



class Scoreboard
{
public:
    std::map<std::string, int> scores;
    std::vector<rankingByScore> leaderboard;
    std::string leaderboardString;
    
    std::string filePath;  // path to the score file (one "<score>\t<name>" line per entry)
    size_t defaultCount = 10; // default number of top scores to display

    void generateLeaderboard(size_t count = 0) {
        // reserve() only sets capacity - it drops nothing. Without the clear() a
        // second call appends the whole map on top of the first batch, and the
        // leaderboard lists every player twice.
        leaderboard.clear();
        leaderboard.reserve(scores.size());
        if (count == 0) count = defaultCount;
        if (count > scores.size()) count = scores.size();

        for (const auto& [name, score] : scores) {
            rankingByScore rbs;
            rbs.Score = score;
            rbs.Name = name;
            leaderboard.push_back(rbs);
        }
        std::partial_sort(leaderboard.begin(), leaderboard.begin() + count, leaderboard.end());
        updateLeaderboardString(count);
    }

    void updateLeaderboardString(size_t count = 0) {
        if (count == 0) count = defaultCount;
        if (count > leaderboard.size()) count = leaderboard.size();
        leaderboardString.clear();
        for (size_t i = 0; i < count; i++) {
            leaderboardString += std::to_string(i + 1) + ". " + leaderboard[i].Name + " " + std::to_string(leaderboard[i].Score) + "\n";
        }
    }

    std::string getLeaderboardString() const {return leaderboardString;}

    // adds the score to the player's existing score.  Creates a new key/value pair if player's name doesn't exist.
    void addScore(std::string name, int score) {scores[name] += score;}
    void changeScore(std::string name, int score) {scores[name] = score;}
    void removeScore(std::string name) {scores.erase(name);}
    void clearScores() {
        scores.clear();
        leaderboard.clear();
        leaderboardString.clear();
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
        // Read line by line, each one "<score>\t<name>". A damaged line is skipped
        // rather than abandoning the load - that resilience is the main reason this
        // format beats JSON here, where one bad byte costs the whole file.
        std::map<std::string, int> parsed;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;

            size_t tabPos = line.find('\t');
            if (tabPos == std::string::npos) {
                std::cerr << "[scoreboard] skipping malformed line: " << line << "\n";
                continue;
            }

            int score = 0;
            try {
                score = std::stoi(line.substr(0, tabPos));
            } catch (const std::exception&) {
                // invalid_argument (not a number) or out_of_range (too big for int).
                std::cerr << "[scoreboard] skipping line with an unreadable score: " << line << "\n";
                continue;
            }

            // Score first, name last: the name is the only field that can hold
            // arbitrary text, so everything past the first tab is the name and
            // nothing needs escaping.
            parsed[line.substr(tabPos + 1)] = score;
        }

        if (parsed.empty()) {
            std::cerr << "[scoreboard] " << filePath << " held no readable scores"
                      << " - keeping the current table\n";
            return false;
        }
        file.close();

        scores = std::move(parsed);
        generateLeaderboard(defaultCount); // also refreshes leaderboardString
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

            // One "<score>\t<name>" line per entry, streamed straight out rather than
            // accumulated - no reason to build the whole file in memory first.
            for (const auto& [name, score] : scores) {
                file << score << '\t' << name << '\n';
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

private:
};
