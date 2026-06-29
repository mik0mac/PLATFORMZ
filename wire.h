// wire.h
//
// The JSON wire contract, mirroring server/server_main.cpp. One side serializes
// input, the other parses state - this file is the client half:
//   serializeInput()  ->  exactly what the server's parseInput() reads
//   applyMessage()    <-  parses a welcome/state packet from buildStatePacket()
//
// State application is sync-by-id, not replace-the-vector, so per-object state
// the server doesn't transmit stays stable. In particular an asteroid's wireframe
// shape is seeded from its startingPosition (see DrawAsteroidShape in shapes.h),
// which is NOT in the packet - so we set it once when the object first appears
// and only update the networked fields (position/velocity/health/...) thereafter.
// Explosions carry no id and are ephemeral visual effects, so they're rebuilt
// each packet.

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <cstdint>

#include "input.h"     // PlayerInput
#include "gamespace.h" // GameSpace + element types

//MARK: Outbound - input
// Matches the server's parseInput() schema. yaw/pitch are absolute (the server
// converts them to a look-delta), so the client tracks them locally and sends
// the current values each frame.
inline std::string serializeInput(uint32_t seq, const PlayerInput& in,
                                  float yaw, float pitch) {
    nlohmann::json j = {
        {"seq",  seq},
        {"mx",   in.moveAxis.x},
        {"mz",   in.moveAxis.y},
        {"jp",   in.jetpack},
        {"grav", in.earthGravity},
        {"fire", in.fire},
        {"yaw",  yaw},
        {"pitch", pitch}
    };
    return j.dump();
}

//MARK: Inbound - parsed result
struct ServerMessage {
    enum class Type { None, Welcome, State, Unknown };
    // Server match phase, carried in every state packet. Drives the networked
    // client's screen: Lobby -> TITLE, Playing -> PLAYING, GameOver -> GAME_OVER.
    enum class Phase { Unknown, Lobby, Playing, GameOver };
    Type     type     = Type::None;
    int      playerId = -1; // Welcome: our assigned slot (index into players[])
    uint32_t tick     = 0;  // State/Welcome
    Phase    phase    = Phase::Unknown; // State only
};

//MARK: Outbound - start request
// Asks the server to start (or restart) a match with the chosen map preset
// (boundary half-size + platform/asteroid counts). Any connected client may send
// it; the server applies the preset, generates a fresh world, and flips to the
// PLAYING phase. First press wins for the round.
inline std::string serializeStart(float half, int platforms, int asteroids) {
    nlohmann::json j = {
        {"type", "start"},
        {"half", half},
        {"plat", platforms},
        {"roid", asteroids}
    };
    return j.dump();
}

namespace wire_detail {

using nlohmann::json;

// Update existing local objects to match the server, creating any unseen ones
// (via make). `make` sets stable fields once; `set` copies the networked fields
// every packet. Does NOT remove locals - use for a fixed, persistent set such as
// player slots. Also the only option for types that aren't move-assignable
// (Player has a const member, so erase(remove_if) won't compile for it).
template <class T, class Make, class Set>
inline void syncByIdNoErase(std::vector<T>& vec, const json& arr, Make make, Set set) {
    for (const auto& jo : arr) {
        uint32_t id = jo.value("id", 0u);
        T* found = nullptr;
        for (auto& e : vec) if (e.id == id) { found = &e; break; }
        if (!found) { vec.push_back(make(jo)); found = &vec.back(); }
        set(*found, jo);
    }
}

// As above, but also drops locals whose id is absent from the packet - for
// collections the server adds to and removes from (asteroids, rockets). Requires
// the element type to be move-assignable.
template <class T, class Make, class Set>
inline void syncById(std::vector<T>& vec, const json& arr, Make make, Set set) {
    std::unordered_set<uint32_t> present;
    for (const auto& jo : arr) present.insert(jo.value("id", 0u));
    syncByIdNoErase(vec, arr, make, set);
    vec.erase(std::remove_if(vec.begin(), vec.end(),
        [&](const T& e) { return present.find(e.id) == present.end(); }), vec.end());
}

inline Vector3 vec3(const json& jo, const char* x, const char* y, const char* z) {
    return { jo.value(x, 0.0f), jo.value(y, 0.0f), jo.value(z, 0.0f) };
}

} // namespace wire_detail

//MARK: Inbound - apply
// Parse one text frame. For a "state" packet, overwrite the GameSpace vectors to
// match the server. Returns what kind of message it was (welcome carries the
// assigned playerId; the caller maps that to "me").
inline ServerMessage applyMessage(const std::string& text, GameSpace& gs) {
    using nlohmann::json;
    using namespace wire_detail;

    ServerMessage msg;
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.contains("type")) { msg.type = ServerMessage::Type::Unknown; return msg; }

    const std::string type = j.value("type", "");
    if (type == "welcome") {
        msg.type     = ServerMessage::Type::Welcome;
        msg.playerId = j.value("playerId", -1);
        msg.tick     = j.value("tick", 0u);
        // Boundary size for this match (the server picked it from the start
        // request's map preset). Sync it so the client renders the right cube -
        // walls aren't in the per-tick state packet.
        if (j.contains("half"))
            gs.getWalls().halfSize = j.value("half", gs.getWalls().halfSize);
        // Platforms are static and arrive once, here. Rebuild from scratch
        // (clear first) so a reconnect that re-sends welcome doesn't duplicate.
        if (j.contains("platforms")) {
            auto& platforms = gs.getPlatforms();
            platforms.clear();
            for (const auto& jo : j["platforms"]) {
                Platform p;
                p.position = p.startingPosition = vec3(jo, "px", "py", "pz");
                p.size = vec3(jo, "sx", "sy", "sz");
                platforms.push_back(p);
            }
        }
        return msg;
    }
    if (type != "state") { msg.type = ServerMessage::Type::Unknown; return msg; }

    msg.type = ServerMessage::Type::State;
    msg.tick = j.value("tick", 0u);
    // Match phase (server-authoritative) - drives the networked client's screen.
    const std::string phase = j.value("phase", "");
    msg.phase = phase == "playing"  ? ServerMessage::Phase::Playing
              : phase == "gameover" ? ServerMessage::Phase::GameOver
              : phase == "lobby"    ? ServerMessage::Phase::Lobby
                                    : ServerMessage::Phase::Unknown;

    // Players - a fixed, persistent set of slots; never erased (also Player
    // isn't move-assignable, so erase(remove_if) wouldn't compile).
    if (j.contains("players")) {
        syncByIdNoErase(gs.getPlayers(), j["players"],
            [](const json& jo) {
                Player p;
                p.id = jo.value("id", 0u);
                p.position = p.startingPosition = vec3(jo, "px", "py", "pz");
                return p;
            },
            [](Player& p, const json& jo) {
                p.position = vec3(jo, "px", "py", "pz");
                p.velocity = vec3(jo, "vx", "vy", "vz");
                p.yaw      = jo.value("yaw", 0.0f);
                p.pitch    = jo.value("pitch", 0.0f);
                p.health   = jo.value("hp", 0);
                p.fuel     = jo.value("fuel", 0.0f);
                p.ammo     = jo.value("ammo", 0);
                p.isAlive  = jo.value("alive", true);
                p.isBot    = jo.value("bot", false); // server-owned: which slots are bots (absent on older packets)
                p.flashTimer = jo.value("flash", 0.0f); // server-driven damage flash (body glow)
                p.isConnected = jo.value("active", true); // hide empty slots client-side
            });
    }

    // Asteroids - startingPosition seeds the wireframe shape, so set it once.
    if (j.contains("asteroids")) {
        syncById(gs.getAsteroids(), j["asteroids"],
            [](const json& jo) {
                Asteroid a;
                a.id = jo.value("id", 0u);
                a.position = a.startingPosition = vec3(jo, "px", "py", "pz");
                a.size = jo.value("size", 1.0f);
                return a;
            },
            [](Asteroid& a, const json& jo) {
                a.position    = vec3(jo, "px", "py", "pz");
                a.velocity    = vec3(jo, "vx", "vy", "vz");
                a.size        = jo.value("size", a.size);
                a.health      = jo.value("hp", a.health);
                a.flashTimer  = jo.value("flash", 0.0f); // server-driven hot-glow damage flash
                a.isDestroyed = jo.value("dead", false);
            });
    }

    // Rockets - server sends velocity but not direction; derive it for drawing.
    if (j.contains("rockets")) {
        syncById(gs.getRockets(), j["rockets"],
            [](const json& jo) {
                Rocket r;
                r.id = jo.value("id", 0u);
                return r;
            },
            [](Rocket& r, const json& jo) {
                r.position    = vec3(jo, "px", "py", "pz");
                r.velocity    = vec3(jo, "vx", "vy", "vz");
                r.direction   = Vector3Normalize(r.velocity);
                r.isDestroyed = jo.value("dead", false);
            });
    }

    // Explosions - no id, ephemeral: rebuild each packet.
    if (j.contains("explosions")) {
        auto& explosions = gs.getExplosions();
        explosions.clear();
        for (const auto& jo : j["explosions"]) {
            Explosion e;
            e.position = vec3(jo, "px", "py", "pz");
            e.radius   = jo.value("r", 0.0f);
            e.isActive = jo.value("active", true);
            explosions.push_back(e);
        }
    }

    // Audio events - one-shot sounds the server emitted this tick. Append (the
    // caller drains + clears each frame); main.cpp filters echoes of events it
    // already predicted locally and pushes the rest to the AudioQueue.
    if (j.contains("audio")) {
        for (const auto& jo : j["audio"]) {
            NetAudioEvent ev;
            ev.fx    = jo.value("fx", 0);
            ev.owner = jo.value("own", 0u);
            ev.pos   = vec3(jo, "px", "py", "pz");
            gs.getAudioEvents().push_back(ev);
        }
    }

    return msg;
}
