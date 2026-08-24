#pragma once

#include <set>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>

// Create an alias for convenience
using json = nlohmann::json;

struct rankingByScore
{
    int Score;
    std::string Name;

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
    int numOfScores = 0;
    std::vector<rankingByScore> leaderboard;
    std::string leaderboardString;
    
    std::string filePath;

    void generateLeaderboard(int count) {
        leaderboard.reserve(scores.size());

        for (const auto& [name, score] : scores) {
            rankingByScore rbs;
            rbs.Score = score;
            rbs.Name = name;
            leaderboard.push_back(rbs);
        }
        std::partial_sort(leaderboard.begin(), leaderboard.begin() + count, leaderboard.end());
        updateLeaderboardString(count);
    }

    void updateLeaderboardString(int count) {
        leaderboardString.clear();
        for (int i = 0; i < count; i++) {
            leaderboardString += std::to_string(i + 1) + ". " + leaderboard[i].Name + " " + std::to_string(leaderboard[i].Score) + "\n";
        }
    }

    std::string getLeaderboardString() const {
        return leaderboardString;
    }

    // adds the score to the player's existing score.  Creates a new key/value pair if player's name doesn't exist.
    void addScore(std::string name, int score) {scores[name] += score; numOfScores = scores.size();}
    void changeScore(std::string name, int score) {scores[name] = score; numOfScores = scores.size();}
    void removeScore(std::string name) {scores.erase(name); numOfScores = scores.size();}
    void clearScores() {
        scores.clear();
        leaderboard.clear();
        leaderboardString.clear();
        numOfScores = 0;
    }


    // MARK: File, load, save
    void setFilePath(const std::string& path) {
        filePath = path;
    }

    void load() {
        // open file stream
        std::ifstream file(filePath);

        // if error
        if (!file.is_open()) {
            std::cerr << "Error opening file for reading!\n";
            return;
        }
        // read data from file
        json data;
        file >> data;
        file.close();

        // clear byScore in memory
        clearScores();

        // parse json data into byScore
        for (const auto& item : data) {
            scores[item["Name"]] = item["Score"];
        }
        numOfScores = scores.size();

        // generate leaderboard and update string
        generateLeaderboard(defaultCount);
        updateLeaderboardString(defaultCount);
    }

        void save() {
            json jsonData = scores;
            
            // Open a file stream
            std::ofstream file(filePath);
            
            if (file.is_open()) {
                // Write formatted JSON (4 spaces indentation)
                file << jsonData.dump(4); 
                file.close();
                std::cout << "JSON file written successfully!\n";
            } else {
                std::cerr << "Error opening file for writing!\n";
            }
        }

    

private:
    static constexpr int defaultCount = 10;

};