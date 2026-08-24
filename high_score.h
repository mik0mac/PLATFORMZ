#pragma once

#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>   // std::rename, std::remove (atomic save)
#include <cstring>  // std::strerror
#include <cerrno>   // errno

// Create an alias for convenience
using json = nlohmann::json;

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

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(rankingByScore, Score, Name)



class Scoreboard
{
public:
    std::map<std::string, int> scores;
    std::vector<rankingByScore> leaderboard;
    std::string leaderboardString;
    
    std::string filePath;  // path to the JSON file for persistence
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
        std::map<std::string, int> parsed;
        try {
            json data;
            file >> data;                                    // throws on malformed JSON
            parsed = data.get<std::map<std::string, int>>();  // throws on the wrong shape
        } catch (const json::exception& e) {
            std::cerr << "[scoreboard] could not read " << filePath << ": " << e.what()
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

            try {
                file << json(scores).dump(4) << "\n"; // 4-space indent, human-editable
            } catch (const json::exception& e) {
                std::cerr << "[scoreboard] could not serialize scores: " << e.what() << "\n";
                std::remove(tmpPath.c_str());
                return false;
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
