#pragma once

#include <set>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

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

// struct rankingByName
// {
//     std::string Name;
//     int Score;
// };

// class that stores the high scores in a multiset.  Read into memory from a file and update as needed.
// Usage:
// 1. Create an instance of HighScore.
// 2. Read the existing high scores from the file if needed.
// 3. Use addScore(<score>, <name>) to add new scores.
// 4. updateTopScoreString() to update the string representation of the top scores once all scores are added.
// 5. save() to save the scores to a file after updating.
// 6. getTopScoreString() to retrieve the current string representation of the top scores.
// 7. clearTopScores() to clear all high scores and the top score string.

class HighScore {
public:
    std::multiset<rankingByScore> byScore;
    // std::set<rankingByName> byName;
    std::string topScoreStr;
    std::string filePath;

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
        clearTopScores();

        // parse json data into byScore
        for (const auto& item : data) {
            rankingByScore rbs;
            rbs.Score = item["Score"];
            rbs.Name = item["Name"];
            // add to byScore
            byScore.insert(rbs);
        }

        // update string
        updateTopScoreString();
    }

    void save() {
        json jsonData = byScore;
        
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

    std::vector<rankingByScore> getTopScores(int count) {
        int numOfEntries = static_cast<int>(byScore.size());
        if (count > numOfEntries) { count = numOfEntries; }
        std::vector<rankingByScore> topScores;
        int i = 0;
        for (const auto& entry : byScore) {
            if (i >= count) {
                break;
            }
            topScores.push_back(entry);
            i++;
        }
        return topScores;
    }

    std::string updateTopScoreString(int count = defaultCount) {
        std::vector<rankingByScore> topScores;
        topScores = getTopScores(count);
        // format string like "<rank>. <playerName> <score>"
        topScoreStr.clear();
        for (int i = 0; i < static_cast<int>(topScores.size()); i++) {
            topScoreStr += std::to_string(i + 1) + ". " + topScores[i].Name + " " + std::to_string(topScores[i].Score) + "\n";
        }
        return topScoreStr;
    }

    std::string getTopScoreString() const {
        return topScoreStr;
    }

    void addScore(int score, std::string name) {
        rankingByScore rbs;
        rbs.Score = score;
        rbs.Name = name;
        
        bool nameExists = false;
        // check if the name already exists in the high score list
        for (rankingByScore s : byScore) {
            if (s.Name == name) {
                byScore.erase(s);
                byScore.insert(rbs);
                nameExists = true;
                break;
            }
        }
        // name was found.  All done.
        if (nameExists) return;
        // name was not in the high score list.  Add it.
        byScore.insert(rbs);
    }

    void clearTopScores() {
        byScore.clear();
        topScoreStr.clear();
    }

private:
    static constexpr int defaultCount = 10;

};