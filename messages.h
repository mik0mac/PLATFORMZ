#pragma once

#include <string>
#include <vector>
#include "raylib.h"

#include "elements.h"
#include "constants.h"


const float DEFAULT_MSG_DURATION = 3.0f; // seconds
const Color DEFAULT_MSG_COLOR = {255, 255, 255, 255}; // white text

enum MessageType {
    MSG_TYPE_LOW_FUEL,
    MSG_TYPE_LOW_AMMO,
    MSG_TYPE_LOW_HEALTH,
    MSG_TYPE_EXPLOSION_HIT,
    MSG_TYPE_PLAYER_COLLISION,
    MSG_TYPE_ASTEROID_COLLISION,
    MSG_TYPE_ELIMINATION,
    MSG_TYPE_ASTEROID_BONUS,
    COUNT
};

class Message {
    public:
    Message(int msg_id, MessageType type, std::string playerAName = "", std::string playerBName = "") {
        initialize(msg_id, type, playerAName, playerBName);
    }
    int msg_id = 0; // unique identifier for the message

    MessageType type;
    bool playerA_Only = false; // if true, only playerA sees this message (e.g., low fuel warning)
    std::string playerA_Name = ""; // optional, for messages involving a player (e.g., hit, collision)
    std::string playerB_Name = ""; // optional, for messages involving two players (e.g., collision, elimination)
    
    std::string text;
    float duration = DEFAULT_MSG_DURATION; // seconds
    float timeRemaining = DEFAULT_MSG_DURATION; // seconds
    Color color;

    void update(float dt) {
        timeRemaining -= dt;
    }
    bool isExpired() const {
        return timeRemaining <= 0.0f;
    }
    // this is for the server to generate a message:
    void initialize(int msg_id, MessageType type, std::string playerAName = "", std::string playerBName = "") {
        this->msg_id = msg_id;
        this->type = type;
        this->playerA_Name = playerAName;
        this->playerB_Name = playerBName;
    }
    // this method is for the local player to generate a message based on the type and optional player names.
    void generate(MessageType type, std::string playerAName = "", std::string playerBName = "") {
        switch (type) {
            case MSG_TYPE_LOW_FUEL:
                text = "Warning: Low Fuel!";
                color = RED;
                break;
            case MSG_TYPE_LOW_AMMO:
                text = "Warning: Low Ammo!";
                color = RED;
                break;
            case MSG_TYPE_LOW_HEALTH:
                text = "Warning: Low Health!";
                color = RED;
                break;
            case MSG_TYPE_EXPLOSION_HIT:
                std::string victim = (!playerBName.empty()) ? playerBName : "someone";
                text = playerAName + " hit " + victim + "!";
                color = RED;
                break;
            case MSG_TYPE_PLAYER_COLLISION:
                std::string victim = (!playerBName.empty()) ? playerBName : "someone";
                text = playerAName + " collided with " + victim + "!";
                color = RED;
                break;
            case MSG_TYPE_ASTEROID_COLLISION:
                text = playerAName + " collided with an asteroid!";
                color = RED;
                break;
            case MSG_TYPE_ELIMINATION:
                std::string victim = (!playerBName.empty()) ? playerBName : "someone";
                text = playerAName + " eliminated " + victim + "!";
                color = RED;
                break;
            case MSG_TYPE_ASTEROID_BONUS:
                text = playerAName + " destroyed an asteroid!";
                color = GREEN;
                break;
            default:
                text = "Unknown message type.";
                color = RED;
                break;
        }
    }
};

// ---------------------------------------------------------------------------
// Message event queue
// ---------------------------------------------------------------------------


// MARK: Audio Queue
class MessageQueue {
public:
    MessageQueue(int id) : playerID(id) {
    }

    void push(const Message& msg) {
        queue.push_back(msg);
    }

    void clearExpiredMessages() {
        for (auto it = queue.begin(); it != queue.end(); ) {
            if (it->isExpired()) {
                it = queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    void update(float dt) {
        for (Message& msg : queue) {
            msg.update(dt);
        }
        clearExpiredMessages();
    }

private:
    std::vector<Message> queue;
};




