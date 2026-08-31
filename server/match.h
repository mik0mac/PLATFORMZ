// server/match.h
//
// Match - one game room: the world, its roster, and everything that ticks.
//
// Until this existed, every field below was a file-scope global in
// server_main.cpp, so one server process could host exactly one match. Hoisting
// them into a struct is the enabling step for hosting many at once (see
// docs/matchmaking-plan.md, epic A). This change on its own hosts exactly one
// Match and is behaviour-identical to what came before it.
//
// WHAT IS *NOT* HERE, on purpose. A Match is the gameplay layer only. It is not
// a server: there is one process, one port, one io_context and one listener pair
// for the whole box, and those stay in server_main.cpp. Anything whose lifetime
// is the process rather than the match stays outside too - the join key, the UDP
// socket, the connection-id counter, and the all-time scoreboard (which spans
// matches by definition).
//
// LOCKING. Two mutexes, exactly as before:
//
//     gameMutex  -> clientMutex  (and udpSendMutex innermost, in server_main.cpp)
//
// They were deliberately NOT merged into one. Several call sites take gameMutex
// and then clientMutex inside it, so a single mutex would self-deadlock on the
// second acquire; and the heartbeat takes clientMutex alone precisely so it
// cannot deadlock against the sim. The ordering is documented at each site and
// still holds - with N matches nothing ever locks two matches at once, so the
// order is per-match and the discipline is unchanged.

#pragma once

#include "../gamespace.h"
#include "../collisions.h"
#include "../input.h"           // PlayerInput
#include "../bot_controller.h"
#include "../options.h"         // MatchOptions defaults via constants.h

#include <boost/asio/ip/udp.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class Session;                  // WS sink; defined in server_main.cpp
enum class Transport { WS, UDP };

//MARK: Client record
// ConnectedClient - server-side record per connection. One record per player
// slot, regardless of transport: a WebSocket client is reached via its Session,
// a UDP client via its source endpoint. Everything else (input, name, slot) is
// transport-agnostic, so the sim loop treats both identically.
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
    boost::asio::ip::udp::endpoint udpEndpoint;
    double        lastSeenSec  = 0.0;
};

//MARK: Match phase
// The world doesn't exist until a client starts a match; after one ends, any
// client can start another. The sim only ticks while PLAYING; LOBBY and GAMEOVER
// are idle (still broadcast, so clients see the phase + connected slots). All
// gameSpace mutation stays on the sim thread: a client "start" message just sets
// startRequested, which the sim loop consumes.
enum class Phase { LOBBY, COUNTDOWN, PLAYING, GAMEOVER };

inline const char* phaseString(Phase p) {
    switch (p) {
        case Phase::COUNTDOWN: return "countdown";
        case Phase::PLAYING:   return "playing";
        case Phase::GAMEOVER:  return "gameover";
        default:               return "lobby";
    }
}

// Seconds on the steady clock - for UDP last-seen stamping / idle reaping.
inline double NowSec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

//MARK: Match
struct Match {
    using Clock = std::chrono::steady_clock;

    // ---- World + sim ----------------------------------------------------
    GameSpace     gameSpace;
    CollisionGrid collisionGrid;
    std::mutex    gameMutex;
    std::atomic<uint32_t> serverTick{0};
    // Drives the bot slots (every player slot with no connected client). Same
    // tree and per-slot state the local client uses, so networked bots == local
    // bots. Sim-thread only; guarded by gameMutex like the rest of gameSpace.
    BotController botController;

    // ---- Phase ----------------------------------------------------------
    std::atomic<Phase> gamePhase{Phase::LOBBY};
    std::atomic<bool>  startRequested{false};
    // Seconds left in the pre-match countdown, published in every state packet so
    // all clients show the same number and drive their fade-ins in lockstep.
    // Written by the sim thread each COUNTDOWN tick, read by the io thread in
    // buildStatePacket.
    std::atomic<float> countdownRemaining{0.0f};

    // Monotonic match counter, bumped when a match is built and published in every
    // state packet. Clients echo the last epoch they saw in each input packet, and
    // the input handler drops anything stamped with a different one - so an input
    // still in flight from the previous match (or from a client that hasn't yet
    // noticed the restart) can never be applied to the new match's spawn state.
    // 0 is reserved: it means "client didn't stamp an epoch" (a build predating
    // this field), which is accepted for compatibility.
    std::atomic<uint32_t> matchEpoch{0};

    // ---- Pending match config (io thread -> sim thread) ------------------
    // Written by the io thread when a "start"/"options" arrives, read by the sim
    // thread when it consumes startRequested (the flag is the synchronization
    // point).
    std::atomic<float> pendingHalf{GAMESPACE_HALF_SIZE};
    std::atomic<int>   pendingPlat{GAMESPACE_NUMBER_OF_PLATFORMS};
    std::atomic<int>   pendingRoid{GAMESPACE_NUMBER_OF_ASTEROIDS};
    std::atomic<int>   pendingPlayers{GAMESPACE_DEFAULT_PLAYERS};
    std::atomic<float> pendingDiff{BOT_DIFFICULTY_DEFAULT};
    std::atomic<float> pendingWallElast{WALL_ELASTICITY_PLAYER};     // OPTIONS WALL ELASTICITY (players only)
    std::atomic<float> pendingPlatElast{PLATFORM_ELASTICITY_PLAYER}; // OPTIONS PLATFORM ELASTICITY (players only)
    std::atomic<float> pendingBoost{1.0f};       // OPTIONS SPEED BOOST
    std::atomic<float> pendingRocketSpeed{1.0f}; // OPTIONS ROCKET VELOCITY
    std::atomic<float> pendingXRadius{1.0f};     // OPTIONS EXPLOSION RADIUS
    std::atomic<float> pendingJThrust{1.0f};     // OPTIONS JETPACK THRUST
    std::atomic<int>   pendingFuelBurn{(int)FUEL_CONSUMPTION_RATE}; // OPTIONS FUEL CONSUMPTION
    std::atomic<int>   pendingFuelRegen{FUEL_REGEN_PCT_DEFAULT};    // OPTIONS FUEL REGEN (% of consumption)
    std::atomic<bool>  pendingWallsEnabled{WALLS_ENABLED};
    std::atomic<bool>  pendingRocketsPhysics{ROCKETS_OBEY_PHYSICS};
    std::atomic<bool>  pendingFriendlyFire{FRIENDLY_FIRE};
    std::atomic<bool>  pendingCoastMode{COAST_MODE};

    // ---- Cached welcome -------------------------------------------------
    // The "static" half of the welcome packet: boundary size + platform layout.
    // These change only when a match (re)generates the world, so it's built ONCE
    // on the sim thread under gameMutex and io threads get a copy - which closes
    // a data race, since welcomes are built on network threads and reading the
    // live platform vector there could tear against a match-start rebuild.
    std::string welcomeStatic;      // JSON:   "half":40.00,"platforms":[...]
    std::string welcomeStaticBin;   // binary: f32 half, u16 count, count*(6 f32)
    std::mutex  welcomeStaticMutex;

    // ---- Roster ---------------------------------------------------------
    // connId -> client record. Keyed by a monotonic id (not a socket pointer) so
    // WS and UDP clients share one registry; the id order is stable
    // (deterministic state serialization). Protected by clientMutex.
    std::map<uint64_t, ConnectedClient> clients;
    // UDP source endpoint -> connId, so an inbound datagram finds its client.
    // UDP only. Protected by clientMutex (same lock as `clients`).
    std::map<boost::asio::ip::udp::endpoint, uint64_t> udpIndex;
    std::mutex clientMutex;

    // ---- Sim-loop carried state -----------------------------------------
    // Were locals of SimulationLoop; they persist across ticks, so they belong to
    // the match now that the tick body is a method.
    Clock::time_point countdownEnd;      // when COUNTDOWN flips to PLAYING (valid only while COUNTDOWN)
    Phase             prevPhase = Phase::LOBBY; // previous tick's phase, for the match-end edge (scoreboard credit)

    // ---- Roster helpers -------------------------------------------------
    int  ClaimFreeSlot();
    void ReapIdleUdpClients();
    std::vector<uint64_t> CompactConnectedSlots();
    std::set<int> gatherClaimedSlots();
    bool isHostConn(uint64_t connId);
    void refreshBotSlots(const std::set<int>& claimed, bool allowBotify);
    void HandleMidMatchLeavers(const std::set<int>& claimed, bool allowBotify, float dt);

    // ---- Packet builders ------------------------------------------------
    std::string buildPlatformsArray();
    void        rebuildWelcomeStatic();
    std::string buildWelcome(int playerId);
    std::string buildWelcomeBinary(int playerId);
    std::string welcomeFor(const ConnectedClient& c);
    std::string buildStateBodyJson(const std::set<int>& connectedSlots);
    std::string buildStatePacket(uint32_t tick, uint32_t lastSeq, const std::string& body);
    std::string buildStateBodyBinary(const std::set<int>& connectedSlots);
    std::string buildStateBinary(uint32_t tick, uint32_t lastSeq, const std::string& body);

    // ---- Tick + dispatch ------------------------------------------------
    // One 60 Hz step: consume a pending start, advance the countdown, reap, tick
    // the sim, then broadcast. Split from the driver loop (SimulationLoop in
    // server_main.cpp) so that loop can later drive several matches per beat.
    void Tick();
    void BroadcastState(uint32_t tick);
    // Dispatch one inbound text frame from a client already in this match's
    // roster. The free HandleClientMessage() in server_main.cpp is the router
    // that finds the match and forwards here.
    void HandleMessage(uint64_t connId, const std::string& msg);
};
