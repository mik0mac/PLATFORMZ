#pragma once

#include <string>
#include <vector>
#include "raylib.h"

#include "elements.h"
#include "constants.h"


const float DEFAULT_MSG_DURATION = 5.0f; // seconds
const Color DEFAULT_MSG_COLOR = {255, 255, 255, 255}; // white text
// const bool DISPLAY_PLAYER_NAMES_IN_ALT_COLOR = true; // if true, display player names in a different color for clarity in messages
// const Color PLAYER_NAME_COLOR = GREEN; // color for player names in messages if bool above is true.

// MARK: Visibility
enum VIS_TYPE {
    VIS_NONE,   // message is suppressed and not shown to any player
    VIS_ALL,    // message is visible to all players
    VIS_PLAYER_A,// message is visible only to player A
    VIS_PLAYER_B, // message is visible only to player B
    VIS_PLAYER_A_PLUS_B, // message is visible to both player A and player B
    VIS_COUNT
};

class Visibility {
    public:
        Visibility(VIS_TYPE type) : type(type) {
            set(type);
        }

        void set(VIS_TYPE type) {
            switch (type) {
                case VIS_NONE:
                    none = true;
                    all = false;
                    playerA = false;
                    playerB = false;
                    break;
                case VIS_ALL:
                    none = false;
                    all = true;
                    playerA = true;
                    playerB = true;
                    break;
                case VIS_PLAYER_A:
                    none = false;
                    all = false;
                    playerA = true;
                    playerB = false;
                    break;
                case VIS_PLAYER_B:
                    none = false;
                    all = false;
                    playerA = false;
                    playerB = true;
                    break;
                case VIS_PLAYER_A_PLUS_B:
                    none = false;
                    all = false;
                    playerA = true;
                    playerB = true;
                    break;
                default:
                    break;
            }
        }

        bool check(std::string player) const {
            if (all) return true;
            if (none) return false;
            if (player == "A" && playerA) return true;
            if (player == "B" && playerB) return true;
            return false;
        }

        VIS_TYPE type;
        bool none = false; // if true, message is suppressed and not shown to any player
        bool all = true;
        bool playerA = true;
        bool playerB = true;
    private:
};

// MARK: Message Class
class Message {
    public:
    Message(MessageType type, const std::string& playerA_Name, const std::string& playerB_Name,
            uint32_t playerA_id = 0, uint32_t playerB_id = 0) :
        type(type), playerA_Name(playerA_Name), playerB_Name(playerB_Name),
        playerA_id(playerA_id), playerB_id(playerB_id) {}
    // int msg_id = 0; // unique identifier for the message

    MessageType type;

    std::string playerA_Name; // Name of the player who triggered the message (e.g., low fuel warning)
    std::string playerB_Name; // Name of the player who is the target of the message (e.g., hit by an explosion)

    // Stable identity for visibility/"YOU" matching. Names are for display only
    // (they can collide - two "PLAYER"s); ids are authoritative. id 0 == "no
    // player" (real player ids start at PLAYER_ID_BASE = 1), used for the empty
    // player B on single-player messages (low fuel/ammo/health).
    uint32_t playerA_id = 0;
    uint32_t playerB_id = 0;

    // visibility.
    Visibility visibility = Visibility(VIS_ALL); // default to visible to all players

    bool visible(uint32_t localPlayerId) const {
        if (localPlayerId == playerA_id) return visibility.check("A");
        if (localPlayerId == playerB_id) return visibility.check("B");
        return visibility.check("C"); // for any other player, check if the message is visible to all
    }
    
    std::string text;
    float duration = DEFAULT_MSG_DURATION; // seconds
    float timeRemaining = DEFAULT_MSG_DURATION; // seconds
    Color color = DEFAULT_MSG_COLOR; // default to white text

    void update(float dt) {
        timeRemaining -= dt;
    }
    bool isExpired() const {
        return timeRemaining <= 0.0f;
    }
    
    // this method is for the local player to generate a message based on the type and optional player names.
    void generate(std::string pa, std::string pb) {
        switch (type) {
            case MSG_TYPE_LOW_FUEL:
                text = "WARNING: LOW FUEL.";
                color = RED;
                visibility.set(VIS_PLAYER_A); // only the player with low fuel sees this message 
                break;
            case MSG_TYPE_LOW_AMMO:
                text = "WARNING: LOW AMMO.";
                color = RED;
                visibility.set(VIS_PLAYER_A); // only the player with low ammo sees this message
                break;
            case MSG_TYPE_LOW_HEALTH:
                text = "WARNING: LOW HEALTH.";
                color = RED;
                visibility.set(VIS_PLAYER_A); // only the player with low health sees this message
                break;
            case MSG_TYPE_EXPLOSION_HIT: {
                std::string victim = (!pb.empty()) ? pb : "SOMEONE";
                text = pa + " HIT " + victim + ".";
                color = DEFAULT_MSG_COLOR;
                // Self-hit test is by id, not name: names can collide (two
                // un-named "PLAYER"s), which would wrongly suppress a real hit
                // between two distinct players. id 0 == no player B (shouldn't
                // happen for a hit), so require a real, equal owner/victim.
                if (playerA_id != 0 && playerA_id == playerB_id) {
                    visibility.set(VIS_NONE); // suppress self-hit messages.
                } else {
                    visibility.set(VIS_PLAYER_A_PLUS_B); // both the player who hit and the player who was hit see this message
                }
                break;
            }
            case MSG_TYPE_PLAYER_COLLISION: {
                std::string victim = (!pb.empty()) ? pb : "SOMEONE";
                text = pa + " COLLIDED WITH " + victim + ".";
                color = DEFAULT_MSG_COLOR;
                visibility.set(VIS_PLAYER_A_PLUS_B); // both players see this message
                break;
            }
            case MSG_TYPE_ASTEROID_COLLISION:
                text = pa + " COLLIDED WITH AN ASTEROID.";
                color = DEFAULT_MSG_COLOR;
                visibility.set(VIS_PLAYER_A); // only the player who collided with the asteroid sees this message
                break;
            case MSG_TYPE_ELIMINATION: {
                std::string victim = (!pb.empty()) ? pb : "SOMEONE";
                text = pa + " ELIMINATED " + victim + ".";
                color = BLUE;
                visibility.set(VIS_ALL); // all players see this message
                break;
            }
            case MSG_TYPE_ASTEROID_BONUS:
                text = "ASTEROID BONUS: +" + std::to_string(ASTEROID_HEALTH_AWARD) 
                + " HEALTH, +" + std::to_string((int)ASTEROID_FUEL_AWARD) 
                + " FUEL, +" + std::to_string(ASTEROID_AMMO_AWARD) + " AMMO.";
                color = GREEN;
                visibility.set(VIS_PLAYER_A); // only the player who destroyed the asteroid sees this message
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


// MARK: Message Queue
class MessageQueue {
public:

    void push(const Message& msg) {
        queue.push_back(msg);
    }

    void remove(int index) {
        if (index >= 0 && index < (int)queue.size()) {
            queue.erase(queue.begin() + index);
        }
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

    // MARK: get messages
    std::vector<Message>& getMessages() { return queue; }

    void update(float dt) {
        for (Message& msg : queue) {
            msg.update(dt);
        }
        clearExpiredMessages();
    }

    // Rendering (the kill-feed HUD) lives in main.cpp as DrawMessageQueue(), not
    // here: this header is compiled into the headless server (via gamespace.h),
    // which has no raylib draw primitives. Keeping draw code out means no
    // PLATFORMZ_SERVER guard is needed. Iterate via getMessages() above.

private:
    std::vector<Message> queue;
};




