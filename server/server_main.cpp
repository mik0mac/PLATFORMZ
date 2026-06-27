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
const float          SERVER_GRAVITY = MOON_GRAVITY; // matches client default

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
};

// -------------------------------------------------------------------------
// Shared game state + client registry
// -------------------------------------------------------------------------
GameSpace     gameSpace;
CollisionGrid collisionGrid;
std::mutex    gameMutex;
std::atomic<uint32_t> serverTick{0};

// ws pointer -> client record. Protected by clientMutex.
// Using map so iteration order is stable (useful for state serialization).
std::map<void*, ConnectedClient> clients;
std::mutex clientMutex;

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
        s += ",\"active\":" + jb(connectedSlots.count(i) > 0); // slot occupied?
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
                std::cout << "Client connected -> player slot " << playerId
                          << ". Active: " << clients.size() << "\n";
            }

            // Send welcome with the assigned slot + the static platform layout
            // (platforms never change after generate(), so this is the only time
            // we send them - the client renders from it instead of generate()).
            std::string welcome = "{\"type\":\"welcome\",\"playerId\":"
                + std::to_string(playerId) + ",\"tick\":"
                + std::to_string(serverTick.load())
                + ",\"platforms\":" + buildPlatformsArray() + "}";
            beast::error_code wec;
            self->ws_.write(net::buffer(welcome), wec);

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
        {
            std::lock_guard<std::mutex> gg(gameMutex);

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

            gameSpace.updatePositions(TICK_DT);
            RunCollisionChecks(gameSpace, collisionGrid);
            gameSpace.updateActiveObjects();
            tick = ++serverTick;
            asteroidCount = gameSpace.getAsteroids().size(); // read under gameMutex
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
        gameSpace.generate();
        std::cout << "GameSpace: "
                  << gameSpace.getAsteroids().size() << " asteroids, "
                  << gameSpace.getPlatforms().size() << " platforms, "
                  << gameSpace.getPlayers().size()   << " player slots\n";
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
