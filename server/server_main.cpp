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
#include "../netbin.h"    // binary state-packet codec (UDP only; keeps it under the MTU)

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>       // read the upgrade request (join-key gate)
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/strand.hpp>  // per-session strands (serialize each Session's handlers)

#include <iostream>
#include <cstdlib>   // getenv (join key)
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <array>
#include <deque>
#include <sstream>
#include <iomanip>

namespace beast     = boost::beast;
namespace http      = boost::beast::http;
namespace websocket = boost::beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;
using udp           = boost::asio::ip::udp;

//MARK: Config
const unsigned short PORT      = 9000;
const float          TICK_RATE = 60.0f;
const float          TICK_DT   = 1.0f / TICK_RATE;
// UDP is connectionless - a client that quits just stops sending. Free its slot
// after this many seconds of silence (mirrors the WS Read()-error cleanup).
// Comfortably above the client's 1s keepalive cadence (and deliberately NOT equal
// to COUNTDOWN_SECONDS) so a brief frame hitch can't cull a live client.
const double         UDP_CLIENT_TIMEOUT = 10.0;
// const float          SERVER_GRAVITY = MOON_GRAVITY; // matches client default

//MARK: JSON out
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

//MARK: Client record
// -------------------------------------------------------------------------
// ConnectedClient - server-side record per connection. One record per player
// slot, regardless of transport: a WebSocket client is reached via its Session,
// a UDP client via its source endpoint. Everything else (input, name, slot) is
// transport-agnostic, so the sim loop treats both identically.
// -------------------------------------------------------------------------
class Session;                          // WS sink; fully defined below.
enum class Transport { WS, UDP };

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

    // Transport + sink. WS uses `session` (shared ownership, so a Send racing a
    // disconnect can never touch a freed Session); UDP uses `udpEndpoint` +
    // `lastSeenSec` (last packet arrival, for idle reaping).
    Transport     transport   = Transport::WS;
    std::shared_ptr<Session> session;
    udp::endpoint udpEndpoint;
    double        lastSeenSec  = 0.0;
};

//MARK: Globals
// -------------------------------------------------------------------------
// Shared game state + client registry
// -------------------------------------------------------------------------
GameSpace     gameSpace;
CollisionGrid collisionGrid;
std::mutex    gameMutex;
std::atomic<uint32_t> serverTick{0};
// Join key gate (#33): set PLATFORMZ_KEY in the server's environment and every
// join attempt must present it - in the ws:// URL query (checked during the
// HTTP upgrade) or in the UDP hello's "key" field. Wrong/missing key gets NO
// reply: to a port scanner a silent port looks like nothing worth probing.
// Empty (unset) = gate off, exactly the old behavior. Read once at startup,
// then only ever read - safe from any thread.
std::string   joinKey;

// True if `provided` passes the join gate.
static bool JoinKeyOk(const std::string& provided) {
    return joinKey.empty() || provided == joinKey;
}

// Pull one query parameter out of an HTTP request target, e.g.
// "/ws?key=banana42&x=1" -> "banana42". Empty if absent. No URL-decoding:
// keys should be URL-safe (letters/digits/dash) to begin with.
static std::string QueryParam(const std::string& target, const std::string& name) {
    auto q = target.find('?');
    if (q == std::string::npos) return "";
    std::string query = target.substr(q + 1);
    std::string prefix = name + "=";
    size_t k = 0;
    while (k != std::string::npos) {
        if (query.compare(k, prefix.size(), prefix) == 0) {
            size_t start = k + prefix.size();
            return query.substr(start, query.find('&', start) - start);
        }
        k = query.find('&', k);
        if (k != std::string::npos) ++k;
    }
    return "";
}
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
enum class Phase { LOBBY, COUNTDOWN, PLAYING, GAMEOVER };
std::atomic<Phase> gamePhase{Phase::LOBBY};
std::atomic<bool>  startRequested{false};
// Seconds left in the pre-match countdown, published in every state packet so all
// clients show the same number and drive their fade-ins in lockstep. Written by the
// sim thread each COUNTDOWN tick, read by the io thread in buildStatePacket.
std::atomic<float> countdownRemaining{0.0f};

// Map preset carried by a start request (boundary half-size + object counts).
// Written by the io thread when a "start" arrives, read by the sim thread when
// it consumes startRequested (the flag is the synchronization point).
std::atomic<float> pendingHalf{GAMESPACE_HALF_SIZE};
std::atomic<int>   pendingPlat{GAMESPACE_NUMBER_OF_PLATFORMS};
std::atomic<int>   pendingRoid{GAMESPACE_NUMBER_OF_ASTEROIDS};
// OPTIONS carried by a start request: requested match size (clamped to connected
// clients at consume) and bot difficulty center. Same io->sim handoff as above.
std::atomic<int>   pendingPlayers{GAMESPACE_NUMBER_OF_PLAYERS};
std::atomic<float> pendingDiff{BOT_DIFFICULTY_DEFAULT};
// OPTIONS bool toggles carried by a start request; defaults from constants.h.
std::atomic<bool>  pendingWallsEnabled{WALLS_ENABLED};
std::atomic<bool>  pendingEgPassThrough{EARTH_GRAVITY_PASS_THROUGH_PLATFORMS};
std::atomic<bool>  pendingRocketsPhysics{ROCKETS_OBEY_PHYSICS};
std::atomic<bool>  pendingFriendlyFire{FRIENDLY_FIRE};

static const char* phaseString(Phase p) {
    switch (p) {
        case Phase::COUNTDOWN: return "countdown";
        case Phase::PLAYING:   return "playing";
        case Phase::GAMEOVER:  return "gameover";
        default:               return "lobby";
    }
}

// connId -> client record. Keyed by a monotonic id (not a socket pointer) so WS
// and UDP clients share one registry; the id order is stable (deterministic state
// serialization). Protected by clientMutex.
std::map<uint64_t, ConnectedClient> clients;
// UDP source endpoint -> connId, so an inbound datagram finds its client. UDP
// only. Protected by clientMutex (same lock as `clients`).
std::map<udp::endpoint, uint64_t>   udpIndex;
std::mutex clientMutex;
std::atomic<uint64_t> nextConnId{1};

// The one server UDP socket, set in main() once the io_context exists. Used by
// SendToClient (sim thread) and the UDP receive handler (io threads).
udp::socket*  g_udp = nullptr;
// Serializes concurrent send_to on g_udp (Asio sockets aren't safe for
// concurrent ops on one object). Always innermost if nested under clientMutex.
std::mutex    udpSendMutex;

// Forward decls: Session::Read and the UDP handler both dispatch through these,
// but their bodies need Session complete (SendToClient calls Session::Send), so
// the definitions live just after the Session class.
static void SendToClient(const ConnectedClient& c, const std::string& msg);
static void HandleClientMessage(uint64_t connId, const std::string& msg);

// Seconds on the steady clock - for UDP last-seen stamping / idle reaping.
static double NowSec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Lowest player slot not owned by a connected client, or -1 if the server is
// full. Caller MUST hold gameMutex (reads players) AND clientMutex (reads clients).
static int ClaimFreeSlot() {
    auto& players = gameSpace.getPlayers();
    std::set<int> claimed;
    for (auto& [cid, c] : clients) claimed.insert(c.playerId);
    for (int i = 0; i < (int)players.size(); i++)
        if (claimed.find(i) == claimed.end()) return i;
    return -1;
}

//MARK: Bot slots
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
    for (auto& [cid, c] : clients) claimed.insert(c.playerId);
    return claimed;
}

// Is this connection the lobby "host"? The host is the client owning the lowest
// player slot (playerId) among all connected clients - the same "player 1 = lowest
// connected slot" rule the client uses to gate its lobby UI. Only the host may
// start a match or change OPTIONS; this is the authoritative backstop behind that
// UI gate. Locks clientMutex itself; returns false for an unknown conn or no
// clients. Self-contained (never touches gameMutex), so no lock-ordering risk.
static bool isHostConn(uint64_t connId) {
    std::lock_guard<std::mutex> lock(clientMutex);
    auto it = clients.find(connId);
    if (it == clients.end()) return false;
    int minSlot = it->second.playerId;
    for (auto& [cid, c] : clients) minSlot = std::min(minSlot, c.playerId);
    return it->second.playerId == minSlot;
}

//MARK: Input parse
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

// Clamp a client-supplied display name to the shared cap (constants.h). The
// stock client's entry field already enforces this; this is the backstop so a
// modified client can't ship a name that overflows everyone else's UI.
static std::string clampName(std::string name) {
    if (name.size() > PLAYER_NAME_MAX_CHARS) name.resize(PLAYER_NAME_MAX_CHARS);
    return name;
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

//MARK: Welcome packet
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

// Cached "static" half of the welcome packet: the boundary size + platform
// layout. These change only when a match (re)generates the world, so we build
// this string ONCE on the sim thread under gameMutex (rebuildWelcomeStatic) and
// hand io threads a copy. buildWelcome then does no gameSpace read at all - which
// closes a data race: welcomes are built on network threads (connect/hello), and
// reading the live platform vector there could tear against the sim thread
// wiping+rebuilding it at match start.
std::string welcomeStatic;          // JSON:   "half":40.00,"platforms":[...]
std::string welcomeStaticBin;       // binary: f32 half, u16 count, count*(6 f32)
std::mutex  welcomeStaticMutex;

// Rebuild the cached welcome fragment(s) from the current world, in both JSON (for
// WebSocket) and binary (for UDP). Caller MUST hold gameMutex (reads walls +
// platforms); runs on the sim thread at startup and after each world (re)gen.
static void rebuildWelcomeStatic() {
    std::string s = "\"half\":" + jf(gameSpace.getWalls().halfSize)
                  + ",\"platforms\":" + buildPlatformsArray();

    std::string bin;
    nb::putF32(bin, gameSpace.getWalls().halfSize);
    auto& platforms = gameSpace.getPlatforms();
    nb::putU16(bin, (uint16_t)platforms.size());
    for (const Platform& p : platforms) {
        nb::putF32(bin, p.position.x); nb::putF32(bin, p.position.y); nb::putF32(bin, p.position.z);
        nb::putF32(bin, p.size.x);     nb::putF32(bin, p.size.y);     nb::putF32(bin, p.size.z);
    }

    std::lock_guard<std::mutex> lk(welcomeStaticMutex);
    welcomeStatic    = std::move(s);
    welcomeStaticBin = std::move(bin);
}

// Welcome packet: the client's assigned slot + the cached static world fragment.
// Sent on connect and re-sent to everyone when a match (re)starts. Safe to call
// from any thread - it only reads the mutex-guarded cache, never gameSpace.
static std::string buildWelcome(int playerId) {
    std::string statik;
    { std::lock_guard<std::mutex> lk(welcomeStaticMutex); statik = welcomeStatic; }
    return "{\"type\":\"welcome\",\"playerId\":" + std::to_string(playerId)
         + ",\"tick\":" + std::to_string(serverTick.load())
         + "," + statik + "}";
}

// Binary welcome for UDP clients: the platform list can exceed the MTU as JSON,
// so pack it (netbin.h). Field order matches the client's applyBinaryWelcome.
static std::string buildWelcomeBinary(int playerId) {
    std::string statik;
    { std::lock_guard<std::mutex> lk(welcomeStaticMutex); statik = welcomeStaticBin; }
    std::string b;
    nb::putU8(b, nb::WELCOME_BIN_VERSION);
    nb::putI32(b, playerId);
    nb::putU32(b, serverTick.load());
    b += statik;   // f32 half, u16 platformCount, platforms
    return b;
}

// Pick the welcome format for a client's transport (binary over UDP, JSON over
// WebSocket) - used at every welcome-send site except the WS-only connect path.
static std::string welcomeFor(const ConnectedClient& c) {
    return c.transport == Transport::UDP ? buildWelcomeBinary(c.playerId)
                                         : buildWelcome(c.playerId);
}

//MARK: State packet
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
    s += ",\"countdown\":" + jf(countdownRemaining.load()); // seconds left in the pre-match countdown (0 unless COUNTDOWN)

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
        s += ",\"spec\":"   + jb(p.isSpectating);    // server-owned: dead player has become a free-fly spectator
        s += ",\"stmr\":"   + jf(p.SpectatingTimer); // post-death spectate countdown (drives the client's greyscale ramp)
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
        s += ",\"vol\":" + jf(audio[i].volumeScale);
        s += "}";
    }
    s += "]";

    // Messages (kill-feed / warnings). Only type + the two player names cross the
    // wire; the client's Message::generate() rebuilds text/color/visibility.
    s += ",\"messages\":[";
    auto& msgs = gameSpace.getMessages();
    for (int i = 0; i < (int)msgs.size(); i++) {
        if (i > 0) s += ",";
        s += "{\"mt\":" + ji((int)msgs[i].type);
        s += ",\"pa\":" + js(msgs[i].playerA_Name);
        s += ",\"pb\":" + js(msgs[i].playerB_Name);
        s += ",\"pai\":" + ju(msgs[i].playerA_id);
        s += ",\"pbi\":" + ju(msgs[i].playerB_id);
        s += "}";
    }
    s += "]";

    // Lobby options (match-wide config), echoed every tick so a change by any
    // client shows live on every client's OPTIONS modal + roster preview.
    s += ",\"opt\":{\"nplayers\":" + ji(pendingPlayers.load());
    s += ",\"diff\":"   + jf(pendingDiff.load());
    s += ",\"walls\":"  + jb(pendingWallsEnabled.load());
    s += ",\"egpt\":"   + jb(pendingEgPassThrough.load());
    s += ",\"phys\":"   + jb(pendingRocketsPhysics.load());
    s += ",\"ff\":"     + jb(pendingFriendlyFire.load());
    s += "}";

    s += "}";
    return s;
}

//MARK: State packet (binary)
// -------------------------------------------------------------------------
// The same state as buildStatePacket, packed as little-endian binary (netbin.h)
// for UDP clients so it fits one datagram (~4x smaller than the JSON). Field
// ORDER here must match the client's applyBinaryState() reader in wire.h.
// Unused-on-the-wire fields are dropped: asteroid/rocket `dead` (the server never
// sends dead=true - destroyed objects just vanish from the set).
// -------------------------------------------------------------------------
static std::string buildStateBinary(uint32_t tick, uint32_t lastSeq,
                                    const std::set<int>& connectedSlots) {
    std::string b;
    b.reserve(768);
    nb::putU8(b, nb::STATE_BIN_VERSION);   // byte 0: tag (not '{'); also the discriminator
    nb::putU32(b, tick);
    nb::putU32(b, lastSeq);
    nb::putU8(b, (uint8_t)gamePhase.load()); // Phase enum: 0 lobby,1 countdown,2 playing,3 gameover
    nb::putF32(b, countdownRemaining.load());

    // Options (match-wide), same values buildStatePacket puts in "opt".
    nb::putU8(b, (uint8_t)pendingPlayers.load());
    nb::putF32(b, pendingDiff.load());
    nb::putU8(b, (uint8_t)((pendingWallsEnabled.load() ? 1 : 0) | (pendingEgPassThrough.load() ? 2 : 0)
                          | (pendingRocketsPhysics.load() ? 4 : 0) | (pendingFriendlyFire.load() ? 8 : 0)));

    // Players (fixed roster; u8 count is plenty).
    auto& players = gameSpace.getPlayers();
    nb::putU8(b, (uint8_t)players.size());
    for (int i = 0; i < (int)players.size(); i++) {
        const Player& p = players[i];
        nb::putU32(b, p.id);
        nb::putF32(b, p.position.x); nb::putF32(b, p.position.y); nb::putF32(b, p.position.z);
        nb::putF32(b, p.velocity.x); nb::putF32(b, p.velocity.y); nb::putF32(b, p.velocity.z);
        nb::putF32(b, p.yaw); nb::putF32(b, p.pitch);
        nb::putI16(b, (int16_t)p.health);
        nb::putF32(b, p.fuel);
        nb::putI16(b, (int16_t)p.ammo);
        nb::putF32(b, p.flashTimer);
        nb::putF32(b, p.SpectatingTimer);
        nb::putI32(b, p.score);
        bool active = connectedSlots.count(i) > 0 || p.isBot; // occupied by a human or bot
        nb::putU8(b, (uint8_t)((p.isAlive ? 1 : 0) | (p.isBot ? 2 : 0) | (active ? 4 : 0) | (p.isSpectating ? 8 : 0)));
        nb::putStr(b, p.name);
    }

    // Asteroids.
    auto& asteroids = gameSpace.getAsteroids();
    nb::putU16(b, (uint16_t)asteroids.size());
    for (const Asteroid& a : asteroids) {
        nb::putU32(b, a.id);
        nb::putF32(b, a.position.x); nb::putF32(b, a.position.y); nb::putF32(b, a.position.z);
        nb::putF32(b, a.velocity.x); nb::putF32(b, a.velocity.y); nb::putF32(b, a.velocity.z);
        nb::putF32(b, a.size);
        nb::putI16(b, (int16_t)a.health);
        nb::putF32(b, a.flashTimer);
    }

    // Rockets.
    auto& rockets = gameSpace.getRockets();
    nb::putU16(b, (uint16_t)rockets.size());
    for (const Rocket& r : rockets) {
        nb::putU32(b, r.id);
        nb::putF32(b, r.position.x); nb::putF32(b, r.position.y); nb::putF32(b, r.position.z);
        nb::putF32(b, r.velocity.x); nb::putF32(b, r.velocity.y); nb::putF32(b, r.velocity.z);
    }

    // Explosions (ephemeral, no id).
    auto& explosions = gameSpace.getExplosions();
    nb::putU16(b, (uint16_t)explosions.size());
    for (const Explosion& e : explosions) {
        nb::putF32(b, e.position.x); nb::putF32(b, e.position.y); nb::putF32(b, e.position.z);
        nb::putF32(b, e.radius);
        nb::putU8(b, e.isActive ? 1 : 0);
    }

    // Audio events (one-shots this tick).
    auto& audio = gameSpace.getAudioEvents();
    nb::putU8(b, (uint8_t)audio.size());
    for (const auto& ev : audio) {
        nb::putU8(b, (uint8_t)ev.fx);
        nb::putU32(b, ev.owner);
        nb::putF32(b, ev.pos.x); nb::putF32(b, ev.pos.y); nb::putF32(b, ev.pos.z);
        nb::putF32(b, ev.volumeScale);
    }

    // Messages (kill-feed / warnings): type + two names + two ids.
    auto& msgs = gameSpace.getMessages();
    nb::putU8(b, (uint8_t)msgs.size());
    for (const auto& m : msgs) {
        nb::putU8(b, (uint8_t)m.type);
        nb::putU32(b, m.playerA_id);
        nb::putU32(b, m.playerB_id);
        nb::putStr(b, m.playerA_Name);
        nb::putStr(b, m.playerB_Name);
    }

    return b;
}

//MARK: Session
// -------------------------------------------------------------------------
// Session - one WebSocket connection, one player slot.
//
// Threading: the Listener accepts each socket onto its own strand, so every
// async handler of this Session runs serialized. Send() (called from the sim
// thread each tick) never touches the socket directly - it posts the message
// onto the strand, where an outbox deque is drained by a single async_write
// chain. This fixes two issues with the old synchronous ws_.write:
//   1. A slow client's full TCP buffer blocked the sim thread (head-of-line:
//      one stalled player lagged the whole match).
//   2. Beast's websocket::stream is not thread-safe, and async_read internally
//      WRITES pong replies to client pings - a sim-thread write could
//      interleave with a pong mid-frame (corrupted frames / rare crash).
// -------------------------------------------------------------------------
class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket) : ws_(std::move(socket)) {}

    void Start() {
        // Read the HTTP upgrade request ourselves (instead of letting
        // async_accept consume it) so the join gate can check ?key= in the URL
        // BEFORE the WebSocket handshake completes. A wrong/missing key just
        // returns: the socket destructs and the connection closes without a
        // single byte answered - a scanner learns nothing.
        http::async_read(ws_.next_layer(), buffer_, req_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return; // not even HTTP; drop silently
                if (!websocket::is_upgrade(self->req_)) return;
                if (!JoinKeyOk(QueryParam(std::string(self->req_.target()), "key"))) {
                    std::cout << "WS join rejected (bad key)\n";
                    return;
                }
                self->Accept();
            });
    }

    // Key passed (or gate off): finish the WebSocket handshake from the
    // already-read upgrade request, then claim a slot and welcome the client.
    void Accept() {
        ws_.async_accept(req_, [self = shared_from_this()](beast::error_code ec) {
            if (ec) { std::cerr << "accept: " << ec.message() << "\n"; return; }

            // Assign player slot - acquire locks in a consistent order:
            // always gameMutex before clientMutex to match SimulationLoop.
            int playerId = -1;
            {
                std::lock_guard<std::mutex> gg(gameMutex);
                std::lock_guard<std::mutex> gc(clientMutex);
                playerId = ClaimFreeSlot();
                if (playerId == -1) {
                    std::cout << "Server full, rejecting connection\n";
                    beast::error_code cec;
                    self->ws_.close(websocket::close_code::try_again_later, cec);
                    return;
                }
                self->connId_ = nextConnId++;
                ConnectedClient c;
                c.playerId  = playerId;
                c.transport = Transport::WS;
                c.session   = self;
                clients[self->connId_] = c;
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
            self->Send(buildWelcome(playerId));

            self->Read();
        });
    }

    // Queue one outbound frame. Safe from any thread: hops onto this session's
    // strand and appends to the outbox; the async_write chain drains it there.
    void Send(const std::string& msg) {
        net::post(ws_.get_executor(),
            [self = shared_from_this(), m = msg]() mutable {
                self->QueueWrite(std::move(m));
            });
    }

private:
    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_; // the upgrade request (read in Start, accepted in Accept)
    uint64_t connId_ = 0;   // this Session's key into `clients` (set in Start)

    // Outbound queue - strand-only state (touched exclusively from handlers
    // running on this session's strand, so no mutex).
    std::deque<std::string> outbox_;
    bool writing_ = false;   // an async_write is in flight (outbox_.front())

    // Cap on queued frames per client. At 60 ticks/s, 64 frames is about one
    // second of backlog - past that the client is stalled (backgrounded tab,
    // congested link) and its queued state packets are stale anyway.
    static constexpr size_t MAX_OUTBOX = 64;

    // Strand-only. Append and kick the write chain if idle. When the cap is
    // hit, drop the OLDEST frame not currently in flight: state packets are
    // absolute, so the freshest ones are the ones worth keeping. (In theory
    // that could shed a queued match-restart welcome, but a client a full
    // second behind is effectively gone - and the hello/refresh path re-sends
    // welcomes anyway.)
    void QueueWrite(std::string msg) {
        if (outbox_.size() >= MAX_OUTBOX)
            outbox_.erase(outbox_.begin() + (writing_ ? 1 : 0));
        outbox_.push_back(std::move(msg));
        if (!writing_) {
            writing_ = true;
            DoWrite();
        }
    }

    // Strand-only. Write outbox_.front(); on completion pop it and continue
    // until the deque is empty. Exactly one async_write is ever outstanding.
    void DoWrite() {
        ws_.async_write(net::buffer(outbox_.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    // Client is gone; Read()'s error path does the cleanup.
                    self->writing_ = false;
                    self->outbox_.clear();
                    return;
                }
                self->outbox_.pop_front();
                if (!self->outbox_.empty()) self->DoWrite();
                else                        self->writing_ = false;
            });
    }

    void Read() {
        ws_.async_read(buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    std::lock_guard<std::mutex> lock(clientMutex);
                    auto it = clients.find(self->connId_);
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

                // All message dispatch (start/name/options/input/hello) is shared
                // with the UDP path - see HandleClientMessage.
                HandleClientMessage(self->connId_, msg);
                self->Read();
            });
    }
};

//MARK: SendToClient
// -------------------------------------------------------------------------
// Ship one text frame to a client over whichever transport it uses. WS goes
// through the Session (ws_.write); UDP is a single datagram via the shared
// socket, serialized by udpSendMutex (Asio sockets aren't safe for concurrent
// send_to). Errors are swallowed - a dead WS client is cleaned up by its Read()
// callback; a dead UDP client is reaped on idle timeout.
// -------------------------------------------------------------------------
static void SendToClient(const ConnectedClient& c, const std::string& msg) {
    if (c.transport == Transport::WS) {
        if (c.session) c.session->Send(msg);
    } else if (g_udp) {
        std::lock_guard<std::mutex> lk(udpSendMutex);
        boost::system::error_code ec;
        if (msg.size() <= nb::UDP_SAFE_DATAGRAM) {
            g_udp->send_to(net::buffer(msg), c.udpEndpoint, 0, ec);
            return;
        }
        // Oversized (in practice: the LARGE-map welcome): split into chunked
        // datagrams that each dodge IP fragmentation - some home routers drop
        // fragmented UDP, which used to make big welcomes undeliverable (#30).
        // Framing + reassembly rules live in netbin.h; the client reassembles
        // in UdpTransport::poll. gen is guarded by udpSendMutex (held here).
        static uint8_t chunkGen = 0;
        uint8_t gen = ++chunkGen;
        size_t count = (msg.size() + nb::CHUNK_PAYLOAD - 1) / nb::CHUNK_PAYLOAD;
        if (count > 255) return; // >300 KB: not a message we ever produce
        for (size_t i = 0; i < count; ++i) {
            std::string chunk;
            chunk.reserve(nb::UDP_SAFE_DATAGRAM);
            nb::putU8(chunk, nb::CHUNK_VERSION);
            nb::putU8(chunk, gen);
            nb::putU8(chunk, (uint8_t)i);
            nb::putU8(chunk, (uint8_t)count);
            chunk += msg.substr(i * nb::CHUNK_PAYLOAD,
                                std::min(nb::CHUNK_PAYLOAD, msg.size() - i * nb::CHUNK_PAYLOAD));
            g_udp->send_to(net::buffer(chunk), c.udpEndpoint, 0, ec);
        }
    }
}

//MARK: Handle client message
// -------------------------------------------------------------------------
// Dispatch one inbound text frame from an already-registered client (WS or UDP).
// Shared by Session::Read and the UDP receive handler; runs on an io thread.
// New-UDP-peer registration (claiming a slot) happens in the UDP handler BEFORE
// this is called, so here the client always exists in `clients`.
// -------------------------------------------------------------------------
static void HandleClientMessage(uint64_t connId, const std::string& msg) {
    //MARK: Msg: hello
    // Handshake / keepalive: (re)send the welcome to this client. UDP clients
    // resend hello until welcomed (unreliable transport); a WS client's hello
    // just re-welcomes harmlessly. A name may ride the hello. buildWelcome reads
    // gameSpace unlocked exactly as the WS connect-welcome does.
    if (msg.find("\"type\":\"hello\"") != std::string::npos) {
        std::lock_guard<std::mutex> lock(clientMutex);
        auto it = clients.find(connId);
        if (it != clients.end()) {
            std::string nm = clampName(parseString(msg, "name"));
            if (!nm.empty()) { it->second.name = nm; it->second.nameDirty = true; }
            SendToClient(it->second, welcomeFor(it->second));
        }
        return;
    }

    //MARK: Msg: start
    // Control message: a client asking to start/restart a match. Only the host
    // (lowest connected slot) may start; ignore it from anyone else. Flagged here
    // and performed by the sim loop so all gameSpace mutation stays on a single
    // thread.
    if (msg.find("\"type\":\"start\"") != std::string::npos) {
        if (!isHostConn(connId)) return; // host-only; non-host clients have no START button, this is the backstop
        // Map preset chosen by the requesting client (first press wins).
        pendingHalf = parseFloat(msg, "half", GAMESPACE_HALF_SIZE);
        pendingPlat = (int)parseUInt(msg, "plat", GAMESPACE_NUMBER_OF_PLATFORMS);
        pendingRoid = (int)parseUInt(msg, "roid", GAMESPACE_NUMBER_OF_ASTEROIDS);
        pendingPlayers = (int)parseUInt(msg, "nplayers", GAMESPACE_NUMBER_OF_PLAYERS);
        pendingDiff = parseFloat(msg, "diff", BOT_DIFFICULTY_DEFAULT);
        pendingWallsEnabled = parseBool(msg, "walls", WALLS_ENABLED);
        pendingEgPassThrough = parseBool(msg, "egpt", EARTH_GRAVITY_PASS_THROUGH_PLATFORMS);
        pendingRocketsPhysics = parseBool(msg, "phys", ROCKETS_OBEY_PHYSICS);
        pendingFriendlyFire = parseBool(msg, "ff", FRIENDLY_FIRE);
        startRequested = true; // release: set after the preset values above
        return;
    }

    //MARK: Msg: name
    // Control message: a client setting its display name. Store it on the client
    // record; the sim loop copies it onto the player slot (keeping gameSpace
    // mutation single-threaded) and it then rides every state packet.
    if (msg.find("\"type\":\"name\"") != std::string::npos) {
        std::lock_guard<std::mutex> lock(clientMutex);
        auto it = clients.find(connId);
        if (it != clients.end()) {
            it->second.name = clampName(parseString(msg, "name"));
            it->second.nameDirty = true;
        }
        return;
    }

    //MARK: Msg: options
    // Control message: a client changing a lobby option (match size, bot
    // difficulty, gameplay toggles). Host-only (match-wide config); ignore from
    // non-host clients. Just update the pending config (no per-client state)
    // WITHOUT starting; the next "start" uses these.
    if (msg.find("\"type\":\"options\"") != std::string::npos) {
        if (!isHostConn(connId)) return; // host-only; matches the client's OPTIONS gating
        pendingPlayers = (int)parseUInt(msg, "nplayers", pendingPlayers.load());
        pendingDiff = parseFloat(msg, "diff", pendingDiff.load());
        pendingWallsEnabled = parseBool(msg, "walls", pendingWallsEnabled.load());
        pendingEgPassThrough = parseBool(msg, "egpt", pendingEgPassThrough.load());
        pendingRocketsPhysics = parseBool(msg, "phys", pendingRocketsPhysics.load());
        pendingFriendlyFire = parseBool(msg, "ff", pendingFriendlyFire.load());
        return;
    }

    //MARK: Msg: ping
    // Keepalive heartbeat (UDP). It carries nothing - its whole job is to prove
    // the client is still alive, and OnDatagram already stamped lastSeenSec before
    // dispatching here. Return early so it doesn't fall through to the input parser
    // (which would store a bogus all-zero input).
    if (msg.find("\"type\":\"ping\"") != std::string::npos) {
        return;
    }

    //MARK: Msg: input
    // Parse input packet and store as this client's latest input. The sim loop
    // reads lastInput each tick; if packets arrive faster than tick rate, only
    // the newest is used (last-write-wins). seq drops stale/out-of-order packets
    // - already the case for TCP, and essential for UDP's reordering.
    {
        std::lock_guard<std::mutex> lock(clientMutex);
        auto it = clients.find(connId);
        if (it != clients.end()) {
            uint32_t seq = parseUInt(msg, "seq", 0);
            if (seq > it->second.lastSeq || !it->second.hasInput) {
                PlayerInput parsed = parseInput(msg);
                // Latch fire: a press only appears in one packet, and the next
                // (fire=false) packet would otherwise overwrite it before the
                // tick reads it. Sticky until consumed.
                if (parsed.fire) it->second.firePending = true;
                it->second.lastInput = parsed;
                it->second.lastSeq = seq;
                it->second.hasInput = true;
            }
        }
    }
}

//MARK: Broadcast
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
    for (auto& [cid, client] : clients) connectedSlots.insert(client.playerId);

    for (auto& [cid, client] : clients) {
        // UDP gets the compact binary state (fits one datagram; MTU-safe over the
        // internet). WebSocket/TCP has no MTU limit, so it keeps the JSON state.
        std::string packet = (client.transport == Transport::UDP)
            ? buildStateBinary(tick, client.lastSeq, connectedSlots)
            : buildStatePacket(tick, client.lastSeq, connectedSlots);
        // Oversize warning (throttled): a binary state over UDP_SAFE_DATAGRAM
        // gets chunked with no retransmit, so any lost chunk drops that whole
        // tick. If this fires steadily, the map preset has too many objects.
        if (client.transport == Transport::UDP && packet.size() > nb::UDP_SAFE_DATAGRAM) {
            static double lastOversizeLog = 0.0;
            double now = NowSec();
            if (now - lastOversizeLog > 1.0) {
                lastOversizeLog = now;
                printf("[server] state packet %zu B > %d (chunking; lossy per-tick)\n",
                       packet.size(), (int)nb::UDP_SAFE_DATAGRAM);
            }
        }
        SendToClient(client, packet);
    }
}

//MARK: Apply input
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

//MARK: Sim loop
// -------------------------------------------------------------------------
// Simulation loop - fixed 60Hz timestep.
// -------------------------------------------------------------------------
void SimulationLoop() {
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::duration<double>;
    auto lastTick  = Clock::now();
    Clock::time_point countdownEnd; // when the pre-match COUNTDOWN flips to PLAYING (valid only while COUNTDOWN)

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

            //MARK: Start match
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
                int want = pendingPlayers.load(), maxClaimed = 0, connectedAtStart = 0;
                {
                    std::lock_guard<std::mutex> gc(clientMutex);
                    connectedAtStart = (int)clients.size();
                    for (auto& [cid, c] : clients) maxClaimed = std::max(maxClaimed, c.playerId + 1);
                }
                want = std::min(std::max(want, maxClaimed), GAMESPACE_NUMBER_OF_PLAYERS);
                gameSpace.setPlayerCount(want);
                // OPTIONS: apply the requesting client's gameplay toggles to the sim
                // before the world is built (collisions read these off GameSpace).
                gameSpace.wallsEnabled = pendingWallsEnabled.load();
                gameSpace.earthGravityPassThroughPlatforms = pendingEgPassThrough.load();
                gameSpace.rocketsObeyPhysics = pendingRocketsPhysics.load();
                gameSpace.friendlyFire = pendingFriendlyFire.load();
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
                // Refresh the cached welcome fragment now that the world exists, so
                // clients that (re)connect during this match get the new platforms.
                rebuildWelcomeStatic();
                // Open the match with a pre-match countdown instead of going live
                // immediately. The world is built but stays FROZEN (the sim body
                // below only ticks in PLAYING/GAMEOVER), so it doesn't move until the
                // count hits zero. justStarted resends the map/welcome now so clients
                // can render the frozen world during the count.
                gamePhase = Phase::COUNTDOWN;
                countdownEnd = now + std::chrono::duration_cast<Clock::duration>(
                                         std::chrono::duration<double>(COUNTDOWN_SECONDS));
                countdownRemaining = COUNTDOWN_SECONDS;
                justStarted = true;
                std::cout << "Match countdown started (" << connectedAtStart << " connected)\n";
            }

            //MARK: Countdown
            // Pre-match countdown: publish the remaining seconds each tick, and flip
            // to PLAYING (unfreezing the world) once the deadline passes. Nothing is
            // simulated while COUNTDOWN (the sim body gates on PLAYING/GAMEOVER).
            if (gamePhase.load() == Phase::COUNTDOWN) {
                double left = Duration(countdownEnd - now).count();
                if (left <= 0.0) {
                    countdownRemaining = 0.0f;
                    gamePhase = Phase::PLAYING;
                    std::cout << "Match started\n";
                } else {
                    countdownRemaining = (float)left;
                }
            }

            // Drop last tick's audio events (already broadcast). They re-accumulate
            // below during input apply + collisions, then BroadcastState() reads
            // and sends them after this lock releases.
            gameSpace.getAudioEvents().clear();
            // Same for messages: drop last tick's already-broadcast kill-feed /
            // warning messages so they don't re-send or accumulate.
            gameSpace.getMessages().clear();

            //MARK: Reap idle UDP clients
            // UDP has no disconnect event, so a client that quit just goes quiet.
            // Free any UDP slot whose last packet is older than the timeout; the
            // bot reconcile below then turns the vacated slot back into a bot.
            // (WS clients are cleaned up by their Read()-error callback instead.)
            {
                std::lock_guard<std::mutex> gc(clientMutex);
                double now = NowSec();
                for (auto it = clients.begin(); it != clients.end(); ) {
                    if (it->second.transport == Transport::UDP &&
                        now - it->second.lastSeenSec > UDP_CLIENT_TIMEOUT) {
                        std::cout << "UDP player " << it->second.playerId
                                  << " timed out. Active: " << (clients.size() - 1) << "\n";
                        udpIndex.erase(it->second.udpEndpoint);
                        it = clients.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            //MARK: Name sync
            // Apply any pending display-name changes onto their player slots. Runs
            // every tick in every phase (lock order gameMutex->clientMutex), so
            // lobby names update live as each client types - and the name persists
            // through the match onto the game-over scoreboard.
            {
                std::lock_guard<std::mutex> gc(clientMutex);
                auto& players = gameSpace.getPlayers();
                for (auto& [cid, client] : clients) {
                    if (!client.nameDirty) continue;
                    if (client.playerId < 0 || client.playerId >= (int)players.size()) continue;
                    players[client.playerId].name = client.name;
                    client.nameDirty = false;
                }
            }

            //MARK: Bot reconcile
            // Reconcile bot ownership every tick in EVERY phase: any slot without a
            // connected client is a bot (named + colored). Running it in the lobby
            // too means the title-screen player list previews the bots that will
            // fill the match (and updates live as humans join/leave); in a match it
            // also handles a mid-match disconnect (a bot reclaims the body) and a
            // join (the bot yields). Cheap - a handful of slots. Bots are only
            // DRIVEN in PLAYING/GAMEOVER (below); in the lobby they just hold a slot.
            { std::lock_guard<std::mutex> gc(clientMutex); refreshBotSlots(gatherClaimedSlots()); }

            //MARK: Tick sim
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
                    for (auto& [cid, client] : clients) {
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

                //MARK: Match end
                // End the match when EITHER every human is dead OR only one player
                // (human or bot) is left standing - so a 2-human match keeps going
                // past the first human's death, and both humans reach GAME OVER on
                // the same phase flip. The single-survivor clause is gated to
                // multi-slot rosters so a solo start (1 slot) doesn't end instantly;
                // it then ends only when the lone human dies (aliveHumans == 0),
                // preserving today's solo behavior. A dead human keeps spectating
                // (client-side greyscale) until the match actually ends here.
                // PLAYING-only: once GAMEOVER we keep simulating (above) but never
                // re-evaluate the end condition.
                if (phase == Phase::PLAYING) {
                    std::lock_guard<std::mutex> gc(clientMutex);
                    auto& players = gameSpace.getPlayers();
                    int aliveHumans = 0;
                    for (auto& [cid, client] : clients) {
                        if (client.playerId >= 0 && client.playerId < (int)players.size()
                            && players[client.playerId].isAlive) aliveHumans++;
                    }
                    int aliveTotal = 0;
                    for (const auto& p : players) if (p.isAlive) aliveTotal++;
                    bool multi = players.size() >= 2;
                    if (aliveHumans == 0 || (multi && aliveTotal <= 1)) {
                        gamePhase = Phase::GAMEOVER;
                        std::cout << "Match over (humans alive " << aliveHumans
                                  << ", total alive " << aliveTotal << ")\n";
                    }
                }
            }

            tick = ++serverTick;
            asteroidCount = gameSpace.getAsteroids().size(); // read under gameMutex
        }

        //MARK: Send tick
        // On (re)start, hand every client the fresh world (their slot + the new
        // platform layout) so they rebuild it. Done off gameMutex like BroadcastState.
        if (justStarted) {
            std::lock_guard<std::mutex> gc(clientMutex);
            for (auto& [cid, client] : clients)
                SendToClient(client, welcomeFor(client));
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

//MARK: Listener
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
        // Accept each connection onto its own strand: with multiple io threads,
        // this serializes all of a Session's handlers (reads, queued writes,
        // pong replies) so they can never run concurrently - see Session.
        acceptor_.async_accept(net::make_strand(ioc_),
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                if (!ec)
                    std::make_shared<Session>(std::move(socket))->Start();
                self->Accept();
            });
    }
};

//MARK: UDP Listener
// -------------------------------------------------------------------------
// UdpListener - the connectionless counterpart to Listener. One shared UDP
// socket receives datagrams from every native client; peers are tracked by
// source endpoint (udpIndex) rather than a per-connection object. A native
// client sends "hello" to claim a slot (resent until it gets a welcome, since
// UDP is unreliable); thereafter its packets carry input/name/options/start.
// Only one async_receive_from is outstanding at a time, so buf_/sender_ are
// never touched concurrently.
// -------------------------------------------------------------------------
class UdpListener : public std::enable_shared_from_this<UdpListener> {
public:
    UdpListener(net::io_context& ioc, udp::endpoint endpoint)
        : socket_(ioc, endpoint) {
        g_udp = &socket_;   // shared send handle for SendToClient / BroadcastState
    }
    void Run() { Receive(); }
private:
    udp::socket             socket_;
    udp::endpoint           sender_;
    std::array<char, 65536> buf_;  // one datagram (IP-reassembled; up to 64 KB)

    void Receive() {
        socket_.async_receive_from(net::buffer(buf_), sender_,
            [self = shared_from_this()](boost::system::error_code ec, std::size_t n) {
                if (!ec && n > 0)
                    self->OnDatagram(std::string(self->buf_.data(), n), self->sender_);
                self->Receive();   // re-arm (single outstanding receive)
            });
    }

    void OnDatagram(const std::string& msg, const udp::endpoint& from) {
        uint64_t connId = 0;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(clientMutex);
            auto idx = udpIndex.find(from);
            if (idx != udpIndex.end()) {
                connId = idx->second;
                known  = true;
                auto it = clients.find(connId);
                if (it != clients.end()) it->second.lastSeenSec = NowSec();
            }
        }
        if (known) { HandleClientMessage(connId, msg); return; }
        // Unknown endpoint: only a hello registers a slot; ignore stray datagrams.
        if (msg.find("\"type\":\"hello\"") != std::string::npos) RegisterPeer(from, msg);
    }

    void RegisterPeer(const udp::endpoint& from, const std::string& helloMsg) {
        // Join gate: a hello without the right key claims nothing and gets NO
        // reply - silence also kills the reflection trick (spoofed hellos can't
        // make us mail welcomes at a victim address the prankster picked).
        if (!JoinKeyOk(parseString(helloMsg, "key"))) {
            std::cout << "UDP join rejected (bad key)\n";
            return;
        }
        int playerId = -1;
        ConnectedClient sink;
        bool ok = false;
        {
            // Lock order gameMutex->clientMutex, matching Session::Start.
            std::lock_guard<std::mutex> gg(gameMutex);
            std::lock_guard<std::mutex> gc(clientMutex);
            playerId = ClaimFreeSlot();
            if (playerId == -1) { std::cout << "Server full, rejecting UDP client\n"; return; }
            uint64_t connId = nextConnId++;
            ConnectedClient c;
            c.playerId    = playerId;
            c.transport   = Transport::UDP;
            c.udpEndpoint = from;
            c.lastSeenSec = NowSec();
            std::string nm = clampName(parseString(helloMsg, "name"));
            if (!nm.empty()) { c.name = nm; c.nameDirty = true; }
            clients[connId] = c;
            udpIndex[from]  = connId;
            sink = c;
            ok = true;
            std::cout << "UDP client connected -> player slot " << playerId
                      << ". Active: " << clients.size() << "\n";
        }
        // Welcome after releasing locks (mirrors Session::Start): buildWelcome
        // reads gameSpace unlocked exactly as the WS connect-welcome does.
        if (ok) SendToClient(sink, welcomeFor(sink));
    }
};

//MARK: main
// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------
int main() {
    std::cout << "PLATFORMZ server | port " << PORT
              << " (TCP/WebSocket + UDP) | " << TICK_RATE << " Hz\n";

    // Join key gate (never printed - it's the secret). Lives in the
    // environment, not the repo: docs/deploy-vultr.md covers setting it on
    // the box; friends receive it inside their invite link / handout build.
    if (const char* k = std::getenv("PLATFORMZ_KEY"); k && *k) {
        joinKey = k;
        std::cout << "Join key: REQUIRED (PLATFORMZ_KEY is set)\n";
    } else {
        std::cout << "Join key: none (open server; set PLATFORMZ_KEY to require one)\n";
    }

    {
        std::lock_guard<std::mutex> lock(gameMutex);
        // Boot into the LOBBY: create player slots only (so clients can connect
        // and be listed), but no world. A client "start" message generates the
        // world and begins the match (see SimulationLoop).
        gameSpace.spawnPlayers();
        rebuildWelcomeStatic(); // seed the cached welcome (empty lobby world) before clients connect
        std::cout << "GameSpace: lobby ready, "
                  << gameSpace.getPlayers().size()
                  << " player slots (waiting for a player to start)\n";
    }

    const int threads = std::max(1u, std::thread::hardware_concurrency());
    net::io_context ioc{threads};

    auto listener = std::make_shared<Listener>(
        ioc, tcp::endpoint{net::ip::make_address("0.0.0.0"), PORT});
    listener->Run();

    // Native clients connect over UDP on the same port (separate TCP/UDP port
    // space, so no conflict with the WebSocket listener above).
    auto udpListener = std::make_shared<UdpListener>(
        ioc, udp::endpoint{udp::v4(), PORT});
    udpListener->Run();

    std::thread(SimulationLoop).detach();

    std::vector<std::thread> pool;
    for (int i = 0; i < threads - 1; i++)
        pool.emplace_back([&ioc] { ioc.run(); });
    ioc.run();

    return 0;
}
