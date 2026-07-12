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
#include "netbin.h"    // binary state-packet decode (UDP)

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
    // client's screen: Lobby -> TITLE, Countdown -> COUNTDOWN, Playing -> PLAYING,
    // GameOver -> GAME_OVER.
    enum class Phase { Unknown, Lobby, Countdown, Playing, GameOver };
    Type     type     = Type::None;
    int      playerId = -1; // Welcome: our assigned slot (index into players[])
    uint32_t tick     = 0;  // State/Welcome
    Phase    phase    = Phase::Unknown; // State only
    float    countdown = 0.0f; // State only: seconds left in the pre-match countdown (0 unless Countdown)

    // Lobby options (match-wide config: match size, bot difficulty, gameplay
    // toggles). The server echoes these in every state packet so a change by any
    // client shows live on all - the client applies them to its OPTIONS modal.
    // hasOptions is false unless the packet carried an "opt" object.
    bool  hasOptions  = false;
    int   optNPlayers = 0;      // match size
    float optDiff     = 0.0f;   // bot difficulty
    bool  optRexpl    = false;  // rockets detonate on boundary wall
    bool  optEgpt     = false;  // fall through platforms under earth gravity
    bool  optPhys     = false;  // rockets obey gravity + inherit shooter velocity
    bool  optFf       = false;  // friendly fire (own blast can self-damage)
};

//MARK: Outbound - start request
// Asks the server to start (or restart) a match with the chosen map preset
// (boundary half-size + platform/asteroid counts). Any connected client may send
// it; the server applies the preset, generates a fresh world, and flips to the
// PLAYING phase. First press wins for the round.
//MARK: Outbound - name
// Tells the server the display name to attach to our player slot. Sent on
// welcome (so the server has a baseline) and whenever the title-screen field
// changes, so the latest typed name wins. The server stores it per slot and
// echoes it back in every state packet's player entry (see applyMessage below),
// so all clients render the same names on the lobby + scoreboard.
inline std::string serializeName(const std::string& name) {
    nlohmann::json j = {
        {"type", "name"},
        {"name", name}
    };
    return j.dump();
}

//MARK: Outbound - hello (handshake / keepalive)
// The client's "I'm here" packet. Sent on connect and re-sent until welcomed.
// Over UDP (connectionless, unreliable) this is what claims a player slot: the
// server assigns a slot and replies with a welcome, and the client keeps sending
// hello until that welcome arrives. Over WebSocket the server already welcomes on
// connect, so a hello just triggers a harmless re-welcome - keeping the client's
// connect path identical for both transports. The name rides along so the server
// has a baseline before the first "name" packet.
inline std::string serializeHello(const std::string& name) {
    nlohmann::json j = {
        {"type", "hello"},
        {"name", name}
    };
    return j.dump();
}

//MARK: Outbound - keepalive
// A tiny heartbeat sent ~1×/sec while connected over UDP. UDP is connectionless,
// so the server frees a client's slot after a few seconds of silence - but the
// client only streams input during active play, going quiet on the lobby /
// countdown / game-over screens. Without this the server would reap a live client
// mid-countdown. Carries nothing; the server just uses its arrival to refresh the
// client's last-seen. WebSocket doesn't need it (TCP keeps the session alive).
inline std::string serializeKeepalive() {
    nlohmann::json j = { {"type", "ping"} };
    return j.dump();
}

// Live lobby-option change (match size, bot difficulty, gameplay toggles). Any
// client may send it whenever a control in the OPTIONS modal changes; the server
// stores it as the pending match config (WITHOUT starting) and echoes it back to
// everyone in the next state packet, so all clients' OPTIONS modals stay in sync.
// Map size isn't here - it's chosen at the START button press (serializeStart).
inline std::string serializeOptions(int nplayers, float diff,
                                    bool rocketsExplode, bool egPassThrough,
                                    bool rocketsPhysics, bool friendlyFire) {
    nlohmann::json j = {
        {"type", "options"},
        {"nplayers", nplayers},
        {"diff", diff},
        {"rexpl", rocketsExplode},
        {"egpt", egPassThrough},
        {"phys", rocketsPhysics},
        {"ff", friendlyFire}
    };
    return j.dump();
}

inline std::string serializeStart(float half, int platforms, int asteroids,
                                  int nplayers, float diff,
                                  bool rocketsExplode, bool egPassThrough,
                                  bool rocketsPhysics, bool friendlyFire) {
    nlohmann::json j = {
        {"type", "start"},
        {"half", half},
        {"plat", platforms},
        {"roid", asteroids},
        {"nplayers", nplayers},        // requested match size (server clamps to connected)
        {"diff", diff},                // bot difficulty center [0..BOT_DIFFICULTY]
        {"rexpl", rocketsExplode},     // OPTIONS: rockets detonate on the boundary wall
        {"egpt", egPassThrough},       // OPTIONS: fall through platforms under earth gravity
        {"phys", rocketsPhysics},      // OPTIONS: rockets obey gravity + inherit shooter velocity
        {"ff", friendlyFire}           // OPTIONS: a player's own blast can self-damage
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

//MARK: Inbound - binary welcome (UDP)
// Decode a binary welcome (server buildWelcomeBinary): assigned slot + the static
// world (boundary size + platform layout). Mirror of the JSON "welcome" branch.
inline ServerMessage applyBinaryWelcome(const std::string& buf, GameSpace& gs) {
    ServerMessage msg;
    msg.type = ServerMessage::Type::Welcome;

    nb::Reader r(buf);
    r.u8();                    // tag (already checked by caller)
    msg.playerId = r.i32();
    msg.tick     = r.u32();
    gs.getWalls().halfSize = r.f32();

    // Platforms arrive once, here. Rebuild from scratch so a reconnect that
    // re-sends welcome doesn't duplicate.
    uint16_t count = r.u16();
    auto& platforms = gs.getPlatforms();
    platforms.clear();
    for (int k = 0; k < count; ++k) {
        Platform p;
        p.position = p.startingPosition = { r.f32(), r.f32(), r.f32() };
        p.size     = { r.f32(), r.f32(), r.f32() };
        platforms.push_back(p);
    }
    return msg;
}

//MARK: Inbound - binary state (UDP)
// Decode a binary state packet (server buildStateBinary) into the GameSpace, the
// mirror of the "state" branch of applyMessage below - same sync-by-id semantics,
// reading fields sequentially from the byte stream instead of JSON. Field ORDER
// must match the server writer exactly.
inline ServerMessage applyBinaryState(const std::string& buf, GameSpace& gs) {
    ServerMessage msg;
    msg.type = ServerMessage::Type::State;

    nb::Reader r(buf);
    r.u8();                      // version tag (already checked by caller)
    msg.tick = r.u32();
    r.u32();                     // seq (our own echoed lastSeq; unused client-side)
    uint8_t phase = r.u8();
    msg.phase = phase == 2 ? ServerMessage::Phase::Playing
              : phase == 1 ? ServerMessage::Phase::Countdown
              : phase == 3 ? ServerMessage::Phase::GameOver
              : phase == 0 ? ServerMessage::Phase::Lobby
                           : ServerMessage::Phase::Unknown;
    msg.countdown = r.f32();

    // Options (present every packet).
    msg.hasOptions  = true;
    msg.optNPlayers = r.u8();
    msg.optDiff     = r.f32();
    uint8_t optFlags = r.u8();
    msg.optRexpl = (optFlags & 1) != 0;
    msg.optEgpt  = (optFlags & 2) != 0;
    msg.optPhys  = (optFlags & 4) != 0;
    msg.optFf    = (optFlags & 8) != 0;

    // Players - fixed slots, never erased; hide slots the server stopped sending.
    {
        auto& players = gs.getPlayers();
        std::unordered_set<uint32_t> present;
        uint8_t count = r.u8();
        for (int k = 0; k < count; ++k) {
            uint32_t id = r.u32();
            present.insert(id);
            Player* p = nullptr;
            for (auto& e : players) if (e.id == id) { p = &e; break; }
            bool isNew = false;
            if (!p) { Player np; np.id = id; players.push_back(np); p = &players.back(); isNew = true; }

            Vector3 pos = { r.f32(), r.f32(), r.f32() };
            Vector3 vel = { r.f32(), r.f32(), r.f32() };
            float yaw = r.f32(), pitch = r.f32();
            int   hp   = r.i16();
            float fuel = r.f32();
            int   ammo = r.i16();
            float flash = r.f32();
            float stmr  = r.f32();
            int   score = r.i32();
            uint8_t flags = r.u8();
            std::string name = r.str();

            p->position = pos;
            if (isNew) p->startingPosition = pos;
            p->velocity      = vel;
            p->yaw           = yaw;
            p->pitch         = pitch;
            p->health        = hp;
            p->fuel          = fuel;
            p->ammo          = ammo;
            p->flashTimer    = flash;
            p->SpectatingTimer = stmr;
            p->score         = score;
            p->isAlive       = (flags & 1) != 0;
            p->isBot         = (flags & 2) != 0;
            p->isConnected   = (flags & 4) != 0;
            p->isSpectating  = (flags & 8) != 0;
            p->name          = name;
        }
        for (Player& p : players)
            if (present.find(p.id) == present.end()) p.isConnected = false;
    }

    // Asteroids - sync by id, drop absent. startingPosition seeds the wire shape.
    {
        auto& asteroids = gs.getAsteroids();
        std::unordered_set<uint32_t> present;
        uint16_t count = r.u16();
        for (int k = 0; k < count; ++k) {
            uint32_t id = r.u32();
            present.insert(id);
            Asteroid* a = nullptr;
            for (auto& e : asteroids) if (e.id == id) { a = &e; break; }
            bool isNew = false;
            if (!a) { Asteroid na; na.id = id; asteroids.push_back(na); a = &asteroids.back(); isNew = true; }

            Vector3 pos = { r.f32(), r.f32(), r.f32() };
            Vector3 vel = { r.f32(), r.f32(), r.f32() };
            float size = r.f32();
            int   hp   = r.i16();
            float flash = r.f32();

            a->position = pos;
            if (isNew) a->startingPosition = pos;
            a->velocity    = vel;
            a->size        = size;
            a->health      = hp;
            a->syncFillAlpha(); // fill alpha tracks health; clients never run takeDamage
            a->flashTimer  = flash;
            a->isDestroyed = false; // server never sends destroyed; they vanish from the set
        }
        asteroids.erase(std::remove_if(asteroids.begin(), asteroids.end(),
            [&](const Asteroid& a) { return present.find(a.id) == present.end(); }), asteroids.end());
    }

    // Rockets - sync by id, drop absent. Direction derived from velocity.
    {
        auto& rockets = gs.getRockets();
        std::unordered_set<uint32_t> present;
        uint16_t count = r.u16();
        for (int k = 0; k < count; ++k) {
            uint32_t id = r.u32();
            present.insert(id);
            Rocket* rk = nullptr;
            for (auto& e : rockets) if (e.id == id) { rk = &e; break; }
            if (!rk) { Rocket nr; nr.id = id; rockets.push_back(nr); rk = &rockets.back(); }

            Vector3 pos = { r.f32(), r.f32(), r.f32() };
            Vector3 vel = { r.f32(), r.f32(), r.f32() };
            rk->position    = pos;
            rk->velocity    = vel;
            rk->direction   = Vector3Normalize(vel);
            rk->isDestroyed = false;
        }
        rockets.erase(std::remove_if(rockets.begin(), rockets.end(),
            [&](const Rocket& rk) { return present.find(rk.id) == present.end(); }), rockets.end());
    }

    // Explosions - ephemeral, rebuilt each packet.
    {
        auto& explosions = gs.getExplosions();
        explosions.clear();
        uint16_t count = r.u16();
        for (int k = 0; k < count; ++k) {
            Explosion e;
            e.position = { r.f32(), r.f32(), r.f32() };
            e.radius   = r.f32();
            e.isActive = r.u8() != 0;
            explosions.push_back(e);
        }
    }

    // Audio events - appended (drained + cleared each frame by main.cpp).
    {
        uint8_t count = r.u8();
        for (int k = 0; k < count; ++k) {
            NetAudioEvent ev;
            ev.fx          = r.u8();
            ev.owner       = r.u32();
            ev.pos         = { r.f32(), r.f32(), r.f32() };
            ev.volumeScale = r.f32();
            gs.getAudioEvents().push_back(ev);
        }
    }

    // Messages - appended; text/color/visibility rebuilt client-side.
    {
        uint8_t count = r.u8();
        for (int k = 0; k < count; ++k) {
            uint8_t mt = r.u8();
            uint32_t pai = r.u32();
            uint32_t pbi = r.u32();
            std::string pa = r.str();
            std::string pb = r.str();
            gs.getMessages().push_back(Message((MessageType)mt, pa, pb, pai, pbi));
        }
    }

    return msg;
}

//MARK: Inbound - apply
// Parse one text frame. For a "state" packet, overwrite the GameSpace vectors to
// match the server. Returns what kind of message it was (welcome carries the
// assigned playerId; the caller maps that to "me").
inline ServerMessage applyMessage(const std::string& text, GameSpace& gs) {
    using nlohmann::json;
    using namespace wire_detail;

    // Binary packet (UDP): byte 0 is a tag, never '{'. Everything the WebSocket
    // path sends (welcome + state) stays JSON and falls through below.
    if (!text.empty()) {
        uint8_t tag = (uint8_t)text[0];
        if (tag == nb::STATE_BIN_VERSION)   return applyBinaryState(text, gs);
        if (tag == nb::WELCOME_BIN_VERSION) return applyBinaryWelcome(text, gs);
    }

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
    msg.phase = phase == "playing"   ? ServerMessage::Phase::Playing
              : phase == "countdown" ? ServerMessage::Phase::Countdown
              : phase == "gameover"  ? ServerMessage::Phase::GameOver
              : phase == "lobby"     ? ServerMessage::Phase::Lobby
                                     : ServerMessage::Phase::Unknown;
    msg.countdown = j.value("countdown", 0.0f); // seconds left (0 unless Countdown)

    // Lobby options snapshot (match-wide). Present every state packet; the client
    // applies these to its OPTIONS modal so any client's change shows live.
    if (j.contains("opt")) {
        const auto& o = j["opt"];
        msg.hasOptions  = true;
        msg.optNPlayers = o.value("nplayers", 0);
        msg.optDiff     = o.value("diff", 0.0f);
        msg.optRexpl    = o.value("rexpl", false);
        msg.optEgpt     = o.value("egpt", false);
        msg.optPhys     = o.value("phys", false);
        msg.optFf       = o.value("ff", false);
    }

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
                p.isSpectating    = jo.value("spec", false); // server-owned: dead player is now a free-fly spectator
                p.SpectatingTimer = jo.value("stmr", p.SpectatingTimer); // drives the client-side greyscale ramp
                p.isBot    = jo.value("bot", false); // server-owned: which slots are bots (absent on older packets)
                p.flashTimer = jo.value("flash", 0.0f); // server-driven damage flash (body glow)
                p.isConnected = jo.value("active", true); // hide empty slots client-side
                p.name     = jo.value("name", std::string("Player")); // server-owned display name
                p.score    = jo.value("score", p.score); // server-owned score (kept stable if absent)
            });
        // Slots the server stopped sending (roster shrank between matches, e.g.
        // lobby bots popped when the match starts) are orphaned locally at their
        // last-known position - which for lobby-only slots is the origin,
        // rendering an inert "phantom" body at world center. We can't erase them
        // (Player has const members, hence syncByIdNoErase), so hide them via the
        // existing isConnected draw gate.
        std::unordered_set<uint32_t> presentIds;
        for (const auto& jo : j["players"]) presentIds.insert(jo.value("id", 0u));
        for (Player& p : gs.getPlayers())
            if (presentIds.find(p.id) == presentIds.end()) p.isConnected = false;
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
                a.syncFillAlpha(); // fill alpha tracks health; clients never run takeDamage
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
            ev.volumeScale = jo.value("vol", 1.0f);
            gs.getAudioEvents().push_back(ev);
        }
    }

    // Messages emitted by the server this tick (kill-feed / warnings). Rebuilt
    // from type + names; generate()/visibility run client-side in main.cpp's
    // draw. Appended (drained + cleared each frame like audio events).
    if (j.contains("messages")) {
        for (const auto& jo : j["messages"]) {
            Message m((MessageType)jo.value("mt", 0),
                      jo.value("pa", std::string()),
                      jo.value("pb", std::string()),
                      jo.value("pai", 0u),
                      jo.value("pbi", 0u));
            gs.getMessages().push_back(m);
        }
    }

    return msg;
}
