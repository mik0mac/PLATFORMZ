// server/server_main.cpp
//
// PLATFORMZ authoritative game server.
//
// Input handling (this step):
//   - Each connected client is assigned a player slot (playerId) on connect.
//   - Clients send JSON input packets each frame.
//   - The sim loop applies each client's latest input to their player via
//     ApplyPlayerInput() from input.h - the exact same function the local
//     client uses, keeping server and client physics identical.
//   - State is broadcast as JSON every tick to all clients.
//
// Wire protocol (text frames, JSON):
//   Client -> Server:  {"seq":N,"mx":0.0,"mz":1.0,"jp":false,"grav":false,"fire":false,"yaw":-1.57,"pitch":0.0}
//   Server -> Client (on connect): {"type":"welcome","playerId":1,"tick":0}
//   Server -> Client (each tick):  {"type":"state","tick":N,"seq":N, "players":[...], "asteroids":[...], ...}
//
// BUILD: make  (from server/ directory)

#include "../gamespace.h"
#include "../collisions.h"
#include "../input.h"     // PlayerInput, ApplyPlayerInput() - reused directly
#include "../bot_controller.h" // shared bot orchestration (same tree/drive as the client)

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <iostream>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <sstream>
#include <iomanip>

namespace beast     = boost::beast;
namespace websocket = boost::beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;

const unsigned short PORT      = 9000;
const float          TICK_RATE = 60.0f;
const float          TICK_DT   = 1.0f / TICK_RATE;
// const float          SERVER_GRAVITY = MOON_GRAVITY; // matches client default

// -------------------------------------------------------------------------
// Minimal JSON helpers - hand-rolled to avoid a heavy dependency.
// Only covers what the state packet needs: floats, ints, bools, arrays.
// -------------------------------------------------------------------------
static std::string jf(float v) {
    // 2 decimal places is sufficient precision for positions/velocities.
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << v;
    return ss.str();
}
static std::string ji(int v)    { return std::to_string(v); }
static std::string ju(uint32_t v) { return std::to_string(v); }
static std::string jb(bool v)  { return v ? "true" : "false"; }
// Quote + escape a string for JSON. Client names are limited to printable
// chars 32-125 (see UiTextField), so only " and \ need escaping.
static std::string js(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// -------------------------------------------------------------------------
// ConnectedClient - server-side record per WebSocket connection.
// -------------------------------------------------------------------------
struct ConnectedClient {
    int      playerId   = -1;   // index into GameSpace::players
    uint32_t lastSeq    = 0;    // sequence number of last processed input
    PlayerInput lastInput{};    // most recent input received; applied each tick
    bool hasInput       = false;// true once first packet arrives
    // Fire is a one-frame edge from the client (IsMouseButtonPressed). Latch it
    // here when any packet reports fire=true, so the trailing fire=false packet
    // can't overwrite the press before the sim tick consumes it. Consumed (and
    // cleared) once per shot in SimulationLoop.
    bool firePending    = false;
    // Display name from the client's "name" message. Stored here (io thread) and
    // copied onto the player slot by the sim loop, so all gameSpace mutation stays
    // single-threaded. nameDirty flags an unapplied change.
    std::string name;
    bool nameDirty      = false;
};

// -------------------------------------------------------------------------
// Shared game state + client registry
// -------------------------------------------------------------------------
GameSpace     gameSpace;
CollisionGrid collisionGrid;
std::mutex    gameMutex;
std::atomic<uint32_t> serverTick{0};
// Drives the bot slots (every player slot with no connected client). Same tree
// and per-slot state the local client uses, so networked bots == local bots.
// Sim-thread only; guarded by gameMutex like the rest of gameSpace.
BotController botController;

//MARK: Match phase
// The world doesn't exist until a client starts a match; after one ends, any
// client can start another. The sim only ticks while PLAYING; LOBBY and GAMEOVER
// are idle (still broadcast, so clients see the phase + connected slots). All
// gameSpace mutation stays on the sim thread: a client "start" message just sets
// startRequested, which the sim loop consumes.
enum class Phase { LOBBY, PLAYING, GAMEOVER };
std::atomic<Phase> gamePhase{Phase::LOBBY};
std::atomic<bool>  startRequested{false};
int matchStartConnected = 0; // connected count at match start (sim thread only); sets the last-man-standing threshold

// Map preset carried by a start request (boundary half-size + object counts).
// Written by the io thread when a "start" arrives, read by the sim thread when
// it consumes startRequested (the flag is the synchronization point).
std::atomic<float> pendingHalf{GAMESPACE_HALF_SIZE};
std::atomic<int>   pendingPlat{GAMESPACE_NUMBER_OF_PLATFORMS};
std::atomic<int>   pendingRoid{GAMESPACE_NUMBER_OF_ASTEROIDS};
// OPTIONS carried by a start request: requested match size (clamped to connected
// clients at consume) and bot difficulty center. Same io->sim handoff as above.
std::atomic<int>   pendingPlayers{GAMESPACE_NUMBER_OF_PLAYERS};
std::atomic<float> pendingDiff{BOT_DIFFICULTY};
// OPTIONS bool toggles carried by a start request; defaults from constants.h.
std::atomic<bool>  pendingRocketsExplode{WALLS_STOP_ROCKETS};
std::atomic<bool>  pendingEgPassThrough{EARTH_GRAVITY_PASS_THROUGH_PLATFORMS};

static const char* phaseString(Phase p) {
    switch (p) {
        case Phase::PLAYING:  return "playing";
        case Phase::GAMEOVER: return "gameover";
        default:              return "lobby";
    }
}

// ws pointer -> client record. Protected by clientMutex.
// Using map so iteration order is stable (useful for state serialization).
std::map<void*, ConnectedClient> clients;
std::mutex clientMutex;

// "Fill empty slots": every player slot NOT owned by a connected client becomes
// a bot; claimed slots are humans. Flipping isBot on/off is all that's needed to
// enable/disable the tree drive for that slot (personalities are seeded once at
// match start by botController.init). Bot slots get a NATO name + magenta color;
// a slot that flips to human keeps its name until the client's name message
// applies (see nameDirty). Caller MUST hold gameMutex; `claimed` is the set of
// client-owned slot indices, gathered by the caller under clientMutex (this
// helper never touches `clients`, so it can't deadlock on the lock order).
static void refreshBotSlots(const std::set<int>& claimed) {
    auto& players = gameSpace.getPlayers();
    for (int i = 0; i < (int)players.size(); ++i) {
        bool bot = claimed.count(i) == 0;
        players[i].isBot = bot;
        if (bot) {
            // Same NATO label local mode shows (slot 1 -> first name). Guarded
            // modulo so a bot at slot 0 (all humans gone) still names cleanly.
            int nameIdx = ((i - 1) % BOT_NAME_COUNT + BOT_NAME_COUNT) % BOT_NAME_COUNT;
            players[i].name          = BOT_NAME_STRINGS[nameIdx];
            players[i].color_outline = BOT_OUTLINE_COLOR;
            players[i].color_fill    = BOT_FILL_COLOR;
        }
    }
}

// Gather the claimed-slot set. Caller must hold clientMutex.
static std::set<int> gatherClaimedSlots() {
    std::set<int> claimed;
    for (auto& [ws, c] : clients) claimed.insert(c.playerId);
    return claimed;
}

// -------------------------------------------------------------------------
// Packet parsing - JSON input from client into PlayerInput.
// Minimal hand-rolled parser: finds keys by string search.
// Robust enough for a known fixed schema; not a general JSON parser.
// -------------------------------------------------------------------------
static float parseFloat(const std::string& json, const std::string& key, float def = 0.0f) {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return def;
    pos += key.size() + 3;
    try { return std::stof(json.substr(pos)); } catch (...) { return def; }
}
static bool parseBool(const std::string& json, const std::string& key, bool def = false) {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return def;
    pos += key.size() + 3;
    return json.substr(pos, 4) == "true";
}
static uint32_t parseUInt(const std::string& json, const std::string& key, uint32_t def = 0) {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return def;
    pos += key.size() + 3;
    try { return (uint32_t)std::stoul(json.substr(pos)); } catch (...) { return def; }
}
// Read a JSON string value ("key":"..."). Unescapes \" and \\ (the only escapes
// nlohmann emits for the client's printable-only names). Stops at the closing ".
static std::string parseString(const std::string& json, const std::string& key,
                               const std::string& def = "") {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return def;
    pos += key.size() + 3;                 // skip past "key":
    if (pos >= json.size() || json[pos] != '"') return def;
    ++pos;                                  // skip the opening quote of the value
    std::string out;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (c == '\\' && pos + 1 < json.size()) { out.push_back(json[++pos]); continue; }
        if (c == '"') break;                // closing quote
        out.push_back(c);
    }
    return out;
}

static PlayerInput parseInput(const std::string& json) {
    PlayerInput in;
    // lookDelta comes as absolute yaw/pitch from client (not mouse delta),
    // converted to a delta relative to the player's current yaw/pitch in
    // ApplyInputToPlayer below - same as the mouse-delta approach but safe
    // over a network where we can't guarantee frame timing matches.
    in.lookDelta.x  = parseFloat(json, "yaw",   0.0f);
    in.lookDelta.y  = parseFloat(json, "pitch",  0.0f);
    in.moveAxis.x   = parseFloat(json, "mx",    0.0f);
    in.moveAxis.y   = parseFloat(json, "mz",    0.0f);
    in.jetpack      = parseBool(json,  "jp",    false);
    in.earthGravity = parseBool(json,  "grav",  false);
    in.fire         = parseBool(json,  "fire",  false);
    return in;
}

// Static-world snapshot - sent once in the welcome packet (platforms never move
// or change after generate(), so there's no reason to put them in every tick).
static std::string buildPlatformsArray() {
    std::string s = "[";
    auto& platforms = gameSpace.getPlatforms();
    for (int i = 0; i < (int)platforms.size(); i++) {
        const Platform& p = platforms[i];
        if (i > 0) s += ",";
        s += "{\"px\":" + jf(p.position.x);
        s += ",\"py\":" + jf(p.position.y);
        s += ",\"pz\":" + jf(p.position.z);
        s += ",\"sx\":" + jf(p.size.x);
        s += ",\"sy\":" + jf(p.size.y);
        s += ",\"sz\":" + jf(p.size.z);
        s += "}";
    }
    s += "]";
    return s;
}

// Welcome packet: the client's assigned slot + the current static platform layout
// (empty in the lobby; populated once a match has generated a world). Sent on
// connect and re-sent to everyone when a match (re)starts so they rebuild it.
static std::string buildWelcome(int playerId) {
    return "{\"type\":\"welcome\",\"playerId\":" + std::to_string(playerId)
         + ",\"tick\":" + std::to_string(serverTick.load())
         + ",\"half\":" + jf(gameSpace.getWalls().halfSize) // boundary size for this match
         + ",\"platforms\":" + buildPlatformsArray() + "}";
}

// -------------------------------------------------------------------------
// State serialization - build the JSON state packet sent to all clients.
// Each client gets the same world state but with their own lastSeq injected.
// `connectedSlots` are the player indices currently occupied by a client; each
// player carries an "active" flag so clients can skip rendering empty slots.
// -------------------------------------------------------------------------
static std::string buildStatePacket(uint32_t tick, uint32_t lastSeq,
                                    const std::set<int>& connectedSlots) {
    std::string s;
    s.reserve(1024);
    s += "{\"type\":\"state\"";
    s += ",\"tick\":"  + ju(tick);
    s += ",\"seq\":"   + ju(lastSeq);
    s += ",\"phase\":\"" + std::string(phaseString(gamePhase.load())) + "\"";

    // Players
    s += ",\"players\":[";
    auto& players = gameSpace.getPlayers();
    for (int i = 0; i < (int)players.size(); i++) {
        const Player& p = players[i];
        if (i > 0) s += ",";
        s += "{\"id\":"     + ju(p.id);
        s += ",\"px\":"     + jf(p.position.x);
        s += ",\"py\":"     + jf(p.position.y);
        s += ",\"pz\":"     + jf(p.position.z);
        s += ",\"vx\":"     + jf(p.velocity.x);
        s += ",\"vy\":"     + jf(p.velocity.y);
        s += ",\"vz\":"     + jf(p.velocity.z);
        s += ",\"yaw\":"    + jf(p.yaw);
        s += ",\"pitch\":"  + jf(p.pitch);
        s += ",\"hp\":"     + ji(p.health);
        s += ",\"fuel\":"   + jf(p.fuel);
        s += ",\"ammo\":"   + ji(p.ammo);
        s += ",\"alive\":"  + jb(p.isAlive);
        s += ",\"bot\":"    + jb(p.isBot); // server-owned bot flag (set for unoccupied slots during a match)
        s += ",\"flash\":"  + jf(p.flashTimer); // damage-flash, so the client can glow a hit body
        // A slot renders if a client occupies it OR a bot drives it; genuinely
        // empty slots (lobby / no bot) stay hidden client-side.
        s += ",\"active\":" + jb(connectedSlots.count(i) > 0 || p.isBot); // slot occupied by human or bot?
        s += ",\"score\":"  + ji(p.score); // server-owned score (credited in collisions)
        s += ",\"name\":"   + js(p.name);  // server-owned display name (from the "name" message)
        s += "}";
    }
    s += "]";

    // Asteroids
    s += ",\"asteroids\":[";
    auto& asteroids = gameSpace.getAsteroids();
    for (int i = 0; i < (int)asteroids.size(); i++) {
        const Asteroid& a = asteroids[i];
        if (i > 0) s += ",";
        s += "{\"id\":"   + ju(a.id);
        s += ",\"px\":"   + jf(a.position.x);
        s += ",\"py\":"   + jf(a.position.y);
        s += ",\"pz\":"   + jf(a.position.z);
        s += ",\"vx\":"   + jf(a.velocity.x);
        s += ",\"vy\":"   + jf(a.velocity.y);
        s += ",\"vz\":"   + jf(a.velocity.z);
        s += ",\"size\":" + jf(a.size);
        s += ",\"hp\":"   + ji(a.health);
        s += ",\"flash\":" + jf(a.flashTimer); // hot-glow damage flash for the client
        s += ",\"dead\":" + jb(a.isDestroyed);
        s += "}";
    }
    s += "]";

    // Rockets
    s += ",\"rockets\":[";
    auto& rockets = gameSpace.getRockets();
    for (int i = 0; i < (int)rockets.size(); i++) {
        const Rocket& r = rockets[i];
        if (i > 0) s += ",";
        s += "{\"id\":"    + ju(r.id);
        s += ",\"px\":"    + jf(r.position.x);
        s += ",\"py\":"    + jf(r.position.y);
        s += ",\"pz\":"    + jf(r.position.z);
        s += ",\"vx\":"    + jf(r.velocity.x);
        s += ",\"vy\":"    + jf(r.velocity.y);
        s += ",\"vz\":"    + jf(r.velocity.z);
        s += ",\"dead\":"  + jb(r.isDestroyed);
        s += "}";
    }
    s += "]";

    // Explosions (visual + splash events - client plays the effect)
    s += ",\"explosions\":[";
    auto& explosions = gameSpace.getExplosions();
    for (int i = 0; i < (int)explosions.size(); i++) {
        const Explosion& e = explosions[i];
        if (i > 0) s += ",";
        s += "{\"px\":"     + jf(e.position.x);
        s += ",\"py\":"     + jf(e.position.y);
        s += ",\"pz\":"     + jf(e.position.z);
        s += ",\"r\":"      + jf(e.radius);
        s += ",\"active\":" + jb(e.isActive);
        s += "}";
    }
    s += "]";

    // Audio events (one-shot sounds: launch/explosion/break/hit/death). Each
    // carries an owner id so clients can skip echoes of events they already
    // predicted locally for themselves. Cleared each tick (see SimulationLoop).
    s += ",\"audio\":[";
    auto& audio = gameSpace.getAudioEvents();
    for (int i = 0; i < (int)audio.size(); i++) {
        if (i > 0) s += ",";
        s += "{\"fx\":"  + ji(audio[i].fx);
        s += ",\"own\":" + ju(audio[i].owner);
        s += ",\"px\":"  + jf(audio[i].pos.x);
        s += ",\"py\":"  + jf(audio[i].pos.y);
        s += ",\"pz\":"  + jf(audio[i].pos.z);
        s += "}";
    }
    s += "]";

    s += "}";
    return s;
}

// -------------------------------------------------------------------------
// Session - one WebSocket connection, one player slot.
// -------------------------------------------------------------------------
class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket) : ws_(std::move(socket)) {}

    void Start() {
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (ec) { std::cerr << "accept: " << ec.message() << "\n"; return; }

            // Assign player slot - acquire locks in a consistent order:
            // always gameMutex before clientMutex to match SimulationLoop.
            int playerId = -1;
            {
                std::lock_guard<std::mutex> gg(gameMutex);
                std::lock_guard<std::mutex> gc(clientMutex);
                auto& players = gameSpace.getPlayers();
                std::set<int> claimed;
                for (auto& [ws, c] : clients) claimed.insert(c.playerId);
                for (int i = 0; i < (int)players.size(); i++) {
                    if (claimed.find(i) == claimed.end()) { playerId = i; break; }
                }
                if (playerId == -1) {
                    std::cout << "Server full, rejecting connection\n";
                    beast::error_code cec;
                    self->ws_.close(websocket::close_code::try_again_later, cec);
                    return;
                }
                ConnectedClient c;
                c.playerId = playerId;
                clients[self.get()] = c;
                // Note: no bot-slot refresh here. Bots exist only during a match
                // (set at match start + reconciled each sim tick), so the lobby
                // stays bot-free and a mid-match join's slot yields on the next
                // tick's reconcile (see SimulationLoop). Marking bots here would
                // also invert the gameMutex->clientMutex order we hold above.
                std::cout << "Client connected -> player slot " << playerId
                          << ". Active: " << clients.size() << "\n";
            }

            // Send welcome with the assigned slot + the current platform layout
            // (empty in the lobby; the client renders from it instead of running
            // generate()). Re-sent to everyone when a match (re)starts.
            beast::error_code wec;
            self->ws_.write(net::buffer(buildWelcome(playerId)), wec);

            self->Read();
        });
    }

    void Send(const std::string& msg) {
        beast::error_code ec;
        ws_.write(net::buffer(msg), ec);
        // Errors here mean the client disconnected; Read() will clean up.
    }

private:
    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;

    void Read() {
        ws_.async_read(buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    std::lock_guard<std::mutex> lock(clientMutex);
                    auto it = clients.find(self.get());
                    if (it != clients.end()) {
                        std::cout << "Player " << it->second.playerId
                                  << " disconnected. Active: "
                                  << (clients.size() - 1) << "\n";
                        clients.erase(it);
                    }
                    return;
                }

                std::string msg = beast::buffers_to_string(self->buffer_.data());
                self->buffer_.consume(self->buffer_.size());

                // Control message: a client asking to start/restart a match. Any
                // client may send it. Flagged here and performed by the sim loop
                // so all gameSpace mutation stays on a single thread.
                if (msg.find("\"type\":\"start\"") != std::string::npos) {
                    // Map preset chosen by the requesting client (first press wins).
                    pendingHalf = parseFloat(msg, "half", GAMESPACE_HALF_SIZE);
                    pendingPlat = (int)parseUInt(msg, "plat", GAMESPACE_NUMBER_OF_PLATFORMS);
                    pendingRoid = (int)parseUInt(msg, "roid", GAMESPACE_NUMBER_OF_ASTEROIDS);
                    pendingPlayers = (int)parseUInt(msg, "nplayers", GAMESPACE_NUMBER_OF_PLAYERS);
                    pendingDiff = parseFloat(msg, "diff", BOT_DIFFICULTY);
                    pendingRocketsExplode = parseBool(msg, "rexpl", WALLS_STOP_ROCKETS);
                    pendingEgPassThrough = parseBool(msg, "egpt", EARTH_GRAVITY_PASS_THROUGH_PLATFORMS);
                    startRequested = true; // release: set after the preset values above
                    self->Read();
                    return;
                }

                // Control message: a client setting its display name. Store it on
                // the client record; the sim loop copies it onto the player slot
                // (keeping gameSpace mutation single-threaded) and it then rides
                // every state packet. Sent on welcome and on each title-screen edit.
                if (msg.find("\"type\":\"name\"") != std::string::npos) {
                    std::lock_guard<std::mutex> lock(clientMutex);
                    auto it = clients.find(self.get());
                    if (it != clients.end()) {
                        it->second.name = parseString(msg, "name");
                        it->second.nameDirty = true;
                    }
                    self->Read();
                    return;
                }

                // Parse input packet and store as this client's latest input.
                // The sim loop reads lastInput each tick; if packets arrive
                // faster than tick rate, only the newest is used (last-write-wins).
                {
                    std::lock_guard<std::mutex> lock(clientMutex);
                    auto it = clients.find(self.get());
                    if (it != clients.end()) {
                        uint32_t seq = parseUInt(msg, "seq", 0);
                        // Discard out-of-order packets
                        if (seq > it->second.lastSeq || !it->second.hasInput) {
                            PlayerInput parsed = parseInput(msg);
                            // Latch fire: a press only appears in one packet, and
                            // the next (fire=false) packet would otherwise overwrite
                            // it before the tick reads it. Sticky until consumed.
                            if (parsed.fire) it->second.firePending = true;
                            it->second.lastInput = parsed;
                            it->second.lastSeq = seq;
                            it->second.hasInput = true;
                        }
                    }
                }

                self->Read();
            });
    }
};

// -------------------------------------------------------------------------
// Broadcast state to all connected clients.
// Each client gets the same world state but with their own lastSeq.
//
// Called from the sim loop AFTER gameMutex is released, on purpose: the
// per-client JSON build and blocking socket writes must not stall the
// simulation (or hold gameMutex while a slow/dead client backs up a write).
// Reading gameSpace here without gameMutex is safe because the sim thread is
// its only mutator and this runs on that same thread, sequentially after the
// locked sim step - so no concurrent writer exists. Only clientMutex is taken
// here, to guard the clients map against connects/disconnects on io threads.
// -------------------------------------------------------------------------
void BroadcastState(uint32_t tick) {
    std::lock_guard<std::mutex> lock(clientMutex);
    // Which player slots are occupied, so clients can hide empty ones.
    std::set<int> connectedSlots;
    for (auto& [ws, client] : clients) connectedSlots.insert(client.playerId);

    for (auto& [ws, client] : clients) {
        std::string packet = buildStatePacket(tick, client.lastSeq, connectedSlots);
        static_cast<Session*>(ws)->Send(packet);
    }
}

// -------------------------------------------------------------------------
// Apply a client's input to their player.
// lookDelta from the network carries absolute yaw/pitch (not a delta), so
// we set directly rather than accumulating - avoids drift from dropped packets.
// -------------------------------------------------------------------------
static void ApplyInputToPlayer(Player& player, const PlayerInput& in,
                               float dt, float gravity) {
    // Network sends absolute yaw/pitch; convert to the delta updateLook() wants.
    // updateLook adds the delta on yaw but SUBTRACTS it on pitch, so the two
    // axes need opposite-signed numerators to both land on the absolute target
    // (yaw += d -> d = target - yaw; pitch -= d -> d = pitch - target). Using
    // target - pitch for both made pitch an unstable x2/tick recurrence that
    // pinned it to the +89 clamp - which is why rockets always fired straight up.
    Vector2 lookDelta{
        (in.lookDelta.x - player.yaw)   / player.lookSensitivity,
        (player.pitch - in.lookDelta.y) / player.lookSensitivity
    };
    PlayerInput adjusted = in;
    adjusted.lookDelta = lookDelta;
    ApplyPlayerInput(player, adjusted, dt, gravity, gameSpace);
}

// -------------------------------------------------------------------------
// Simulation loop - fixed 60Hz timestep.
// -------------------------------------------------------------------------
void SimulationLoop() {
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::duration<double>;
    auto lastTick  = Clock::now();

    while (true) {
        auto now     = Clock::now();
        auto elapsed = Duration(now - lastTick).count();
        if (elapsed < TICK_DT) {
            std::this_thread::sleep_for(std::chrono::microseconds(
                (int)((TICK_DT - elapsed) * 900000)));
            continue;
        }
        lastTick = now;

        uint32_t tick;
        size_t   asteroidCount = 0;
        bool     justStarted = false; // a match began this tick -> resend welcomes below
        {
            std::lock_guard<std::mutex> gg(gameMutex);

            // Consume a pending start/restart request: build a fresh world and
            // reset the existing player slots (ids stay stable so connected
            // clients keep their slot mapping across a restart), then begin.
            if (startRequested.exchange(false)) {
                gameSpace.configureMap(pendingHalf.load(), pendingPlat.load(), pendingRoid.load());
                // Resize the roster to the requested match size, but never below the
                // highest connected client's slot (no connected player loses their
                // body). Empty slots become bots via the per-tick reconcile. Do this
                // BEFORE generate so resetPlayersForMatch/placePlayersSpread size to
                // the final count. (lock order gameMutex->clientMutex preserved.)
                int want = pendingPlayers.load(), maxClaimed = 0;
                {
                    std::lock_guard<std::mutex> gc(clientMutex);
                    matchStartConnected = (int)clients.size();
                    for (auto& [ws, c] : clients) maxClaimed = std::max(maxClaimed, c.playerId + 1);
                }
                want = std::min(std::max(want, maxClaimed), GAMESPACE_NUMBER_OF_PLAYERS);
                gameSpace.setPlayerCount(want);
                // OPTIONS: apply the requesting client's gameplay toggles to the sim
                // before the world is built (collisions read these off GameSpace).
                gameSpace.wallsStopRockets = pendingRocketsExplode.load();
                gameSpace.earthGravityPassThroughPlatforms = pendingEgPassThrough.load();
                // Issue #5 order: platforms -> players (spread) -> asteroids
                // (buffered away from the placed players). Same sequence the local
                // client's generate() uses, so both modes build worlds identically.
                gameSpace.generatePlatforms();
                gameSpace.resetPlayersForMatch();
                gameSpace.generateAsteroids();
                // Seed every slot's bot personality once for this match (stable per
                // slot id) at the requested difficulty. Which slots are bots is set
                // by the per-tick reconcile above; drive() reads the profiles here.
                botController.init(gameSpace.getPlayers(), pendingDiff.load());
                gamePhase = Phase::PLAYING;
                justStarted = true;
                std::cout << "Match started (" << matchStartConnected << " connected)\n";
            }

            // Drop last tick's audio events (already broadcast). They re-accumulate
            // below during input apply + collisions, then BroadcastState() reads
            // and sends them after this lock releases.
            gameSpace.getAudioEvents().clear();

            // Apply any pending display-name changes onto their player slots. Runs
            // every tick in every phase (lock order gameMutex->clientMutex), so
            // lobby names update live as each client types - and the name persists
            // through the match onto the game-over scoreboard.
            {
                std::lock_guard<std::mutex> gc(clientMutex);
                auto& players = gameSpace.getPlayers();
                for (auto& [ws, client] : clients) {
                    if (!client.nameDirty) continue;
                    if (client.playerId < 0 || client.playerId >= (int)players.size()) continue;
                    players[client.playerId].name = client.name;
                    client.nameDirty = false;
                }
            }

            // Reconcile bot ownership every tick in EVERY phase: any slot without a
            // connected client is a bot (named + colored). Running it in the lobby
            // too means the title-screen player list previews the bots that will
            // fill the match (and updates live as humans join/leave); in a match it
            // also handles a mid-match disconnect (a bot reclaims the body) and a
            // join (the bot yields). Cheap - a handful of slots. Bots are only
            // DRIVEN in PLAYING/GAMEOVER (below); in the lobby they just hold a slot.
            { std::lock_guard<std::mutex> gc(clientMutex); refreshBotSlots(gatherClaimedSlots()); }

            // Keep simulating through PLAYING *and* GAMEOVER; only LOBBY (no world
            // yet) is idle. The GAMEOVER sim keeps the world moving during the
            // client's game-over countdown so networked play matches local, where
            // the sim runs every frame of that countdown (issue #2). The match-end
            // detection below stays PLAYING-only so it fires exactly once.
            Phase phase = gamePhase.load();
            if (phase == Phase::PLAYING || phase == Phase::GAMEOVER) {
                // Apply each client's latest input to their player slot
                {
                    std::lock_guard<std::mutex> gc(clientMutex);
                    auto& players = gameSpace.getPlayers();
                    for (auto& [ws, client] : clients) {
                        if (!client.hasInput) continue;
                        if (client.playerId < 0 || client.playerId >= (int)players.size()) continue;
                        Player& player = players[client.playerId];
                        float gravity = client.lastInput.earthGravity ? EARTH_GRAVITY : MOON_GRAVITY;
                        // Consume the latched fire edge exactly once (shoot() still
                        // gates on ammo/cooldown), then clear it so a held button
                        // doesn't re-fire every tick from a stale lastInput.fire.
                        PlayerInput input = client.lastInput;
                        input.fire = client.firePending;
                        client.firePending = false;
                        ApplyInputToPlayer(player, input, TICK_DT, gravity);
                    }
                }

                // Drive every bot slot (unoccupied slots) through the behaviour
                // tree, straight into ApplyPlayerInput - NOT ApplyInputToPlayer,
                // whose absolute-aim conversion is only for network clients. Bots
                // emit per-frame deltas exactly like the local client's drive loop.
                botController.drive(gameSpace, TICK_DT);

                gameSpace.updatePositions(TICK_DT);
                RunCollisionChecks(gameSpace, collisionGrid);
                gameSpace.updateActiveObjects();

                // Last-player-standing: end the match when <= threshold connected
                // players remain alive. threshold is 1 for a multi-player match and
                // 0 for a solo start, so a lone player's match ends only when they
                // die (not instantly). Only connected slots count - empty slots are
                // inert and never "win". PLAYING-only: once GAMEOVER we keep the
                // world simulating (above) but never re-evaluate the end condition.
                if (phase == Phase::PLAYING) {
                    std::lock_guard<std::mutex> gc(clientMutex);
                    auto& players = gameSpace.getPlayers();
                    int aliveConnected = 0;
                    for (auto& [ws, client] : clients) {
                        if (client.playerId >= 0 && client.playerId < (int)players.size()
                            && players[client.playerId].isAlive) aliveConnected++;
                    }
                    int threshold = matchStartConnected >= 2 ? 1 : 0;
                    if (aliveConnected <= threshold) {
                        gamePhase = Phase::GAMEOVER;
                        std::cout << "Match over (alive " << aliveConnected << ")\n";
                    }
                }
            }

            tick = ++serverTick;
            asteroidCount = gameSpace.getAsteroids().size(); // read under gameMutex
        }

        // On (re)start, hand every client the fresh world (their slot + the new
        // platform layout) so they rebuild it. Done off gameMutex like BroadcastState.
        if (justStarted) {
            std::lock_guard<std::mutex> gc(clientMutex);
            for (auto& [ws, client] : clients)
                static_cast<Session*>(ws)->Send(buildWelcome(client.playerId));
        }

        // Broadcast authoritative state to all clients every tick.
        // At 60Hz this is ~60 packets/sec per client. For 2 players the
        // bandwidth is trivial; revisit delta-compression if player count grows.
        BroadcastState(tick);

        // Heartbeat (~1/sec) so the Actions live log shows the sim is alive.
        // clientMutex is taken alone here (not nested under gameMutex) so it
        // can't deadlock. std::endl flushes - stdout to a file is fully
        // buffered, so without the flush this wouldn't appear under `tail -f`.
        if (tick % 60 == 0) {
            int connected;
            { std::lock_guard<std::mutex> gc(clientMutex); connected = (int)clients.size(); }
            std::cout << "tick " << tick << "  players " << connected
                      << "  asteroids " << asteroidCount << std::endl;
        }
    }
}

// -------------------------------------------------------------------------
// Listener
// -------------------------------------------------------------------------
class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(net::io_context& ioc, tcp::endpoint endpoint)
        : ioc_(ioc), acceptor_(ioc) {
        beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        acceptor_.bind(endpoint, ec);
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) std::cerr << "Listener: " << ec.message() << "\n";
    }
    void Run() { Accept(); }
private:
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    void Accept() {
        acceptor_.async_accept(
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                if (!ec)
                    std::make_shared<Session>(std::move(socket))->Start();
                self->Accept();
            });
    }
};

// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------
int main() {
    std::cout << "PLATFORMZ server | port " << PORT
              << " | " << TICK_RATE << " Hz\n";

    {
        std::lock_guard<std::mutex> lock(gameMutex);
        // Boot into the LOBBY: create player slots only (so clients can connect
        // and be listed), but no world. A client "start" message generates the
        // world and begins the match (see SimulationLoop).
        gameSpace.spawnPlayers();
        std::cout << "GameSpace: lobby ready, "
                  << gameSpace.getPlayers().size()
                  << " player slots (waiting for a player to start)\n";
    }

    const int threads = std::max(1u, std::thread::hardware_concurrency());
    net::io_context ioc{threads};

    auto listener = std::make_shared<Listener>(
        ioc, tcp::endpoint{net::ip::make_address("0.0.0.0"), PORT});
    listener->Run();

    std::thread(SimulationLoop).detach();

    std::vector<std::thread> pool;
    for (int i = 0; i < threads - 1; i++)
        pool.emplace_back([&ioc] { ioc.run(); });
    ioc.run();

    return 0;
}
