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
//   Client -> Server:  {"seq":N,"ep":M,"mx":0.0,"mz":1.0,"jp":false,"grav":false,"fire":false,"yaw":-1.57,"pitch":0.0}
//   Server -> Client (on connect): {"type":"welcome","playerId":1,"tick":0}
//   Server -> Client (each tick):  {"type":"state","tick":N,"seq":N,"ep":M, "players":[...], "asteroids":[...], ...}
//
// "ep" is the match epoch: the server bumps it per match and echoes it in every
// state packet; the client stamps the newest one it saw on each input. Input
// carrying a different epoch is dropped, so an input built for the previous
// match can't land on the new match's spawn state. "ep":0 means "unstamped"
// and is accepted, so a client build predating the field still works.
//
// BUILD: make  (from server/ directory)

#include "../gamespace.h"
#include "../collisions.h"
#include "../input.h"     // PlayerInput, ApplyPlayerInput() - reused directly
#include "../bot_controller.h" // shared bot orchestration (same tree/drive as the client)
#include "../netbin.h"    // binary state-packet codec (UDP only; keeps it under the MTU)
#include "jsonmin.h"      // jf/ji/ju/jb/js - the shared JSON writers
#include "crypto.h"       // HMAC-SHA256 + constant-time compare (E1's cookie, later D3's token)
#include "bucket.h"       // pz::Bucket - the token bucket behind every rate limit (E1, E2)
#include "identity.h"     // ident::Mint / Verify - the server-issued identity token (D3)
#include "match.h"        // Match: the world, its roster, and everything that ticks
#include "registry.h"     // MatchRegistry: which rooms exist, and their lifecycle
#include "../scoreboard.h" // cumulative all-time score table, persisted between runs

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>       // read the upgrade request (join-key gate)
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/strand.hpp>  // per-session strands (serialize each Session's handlers)

#include <iostream>
#include <cstdlib>   // getenv (join key)
#include <cstring>   // strcmp (join refusal tokens)
#include <csignal>   // SIGTERM/SIGINT - flush the scoreboard before exiting
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
#include <algorithm> // std::sort (match-start slot compaction)
#include <climits>   // INT_MAX (host migration picks the lowest remaining slot)
#include <sstream>
#include <iomanip>

namespace beast     = boost::beast;
namespace http      = boost::beast::http;
namespace websocket = boost::beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;
using udp           = boost::asio::ip::udp;

//MARK: Config
// Listen port, both TCP/WebSocket and UDP. Overridable with PLATFORMZ_PORT so a
// second instance can run alongside the live one - measuring a server (A4) means
// running it under load, and doing that on the deployment port would mean taking
// the real one down first. Not const: set once in main() before either listener
// is constructed, then only read.
unsigned short       PORT      = 9000;
const unsigned short PORT_DEFAULT = 9000;
const float          TICK_RATE = 60.0f;
const float          TICK_DT   = 1.0f / TICK_RATE;
// UDP is connectionless - a client that quits just stops sending. Free its slot
// after this many seconds of silence (mirrors the WS Read()-error cleanup).
// Comfortably above the client's 1s keepalive cadence (and deliberately NOT equal
// to COUNTDOWN_SECONDS) so a brief frame hitch can't cull a live client.
const double         UDP_CLIENT_TIMEOUT = 10.0;
// Shorter timeout while nobody's mid-action: an ungraceful lobby disconnect
// (crash/force-quit - no "goodbye" possible) shouldn't sit around looking
// connected for as long as a mid-match one would need to survive a hitch.
const double         UDP_CLIENT_TIMEOUT_LOBBY = 3.0;
// How quiet an existing connection must be before a hello carrying the same
// clientId is allowed to take its place. Above the client's own ~3 s
// silence-reset would make reconnects miss their body; at zero, a second client
// launched from the same machine (same profile, same id) would evict the first.
const double         UDP_TWIN_SUPERSEDE_SEC = 2.0;
// const float          SERVER_GRAVITY = MOON_GRAVITY; // matches client default

//MARK: Globals
// -------------------------------------------------------------------------
// Shared game state + client registry
// -------------------------------------------------------------------------
// Cumulative all-time score table (scoreboard.h). Credited once per match on the
// PLAYING -> GAMEOVER edge in SimulationLoop, then saved and broadcast. Its own
// mutex: it is touched by the sim thread (credit) and by io threads (the join
// path builds a leaderboard packet), and it is unrelated to the world state.
// Lock order where both are needed: gameMutex -> clientMutex -> scoreboardMutex.
Scoreboard    scoreboard;
std::mutex    scoreboardMutex;
// Rows on the board. Nine human slots and one bot line, in practice - the bots
// share a single identity and are capped at one row between them.
const size_t  SCORES_BOARD_ROWS = 10;
// PLATFORMZ_SCORES_OFFICIAL_ONLY: show only runs from official rooms.
//
// Filters on READ, never on record. Filtering at record time would throw the
// custom runs away for good, so turning it back off would reveal an empty board;
// filtering here makes the switch free in both directions and retroactive on rows
// already on disk. Set once at boot, then only read.
bool          g_scoresOfficialOnly = false;
// Set when a credit changes the table, cleared when the driver loop writes it
// out. A save rewrites the WHOLE file, and with several matches running their
// ends bunch together - twelve rooms finishing inside a second used to mean
// twelve full rewrites. Atomic because the sim thread sets it and the driver
// loop clears it; it guards nothing, so it needs no mutex of its own.
std::atomic<bool> scoreboardDirty{false};
// How long a credit may sit unwritten. The window is what a crash costs, and it
// is bounded by something else anyway: the match that produced those points took
// minutes, so five seconds of exposure is not the risk worth optimising.
const int SCORES_FLUSH_SEC = 5;

// Set by the signal handler, read by the driver loop. A handler may not lock a
// mutex, allocate, or write a file, so all it does here is say "stop"; the
// saving happens back on the loop where it is safe.
//
// A LOCK-FREE std::atomic, not `volatile sig_atomic_t`. The latter is what the C
// standard blesses for a handler - but only for talking to the thread it
// interrupted, and this is not that: the signal is delivered on whichever io
// thread happens to be running, while the SIM thread reads it. That is ordinary
// cross-thread communication, `volatile` gives it no happens-before edge, and
// ThreadSanitizer called it a data race the moment it was written. C++ permits a
// handler to touch an always-lock-free atomic, which this is on every platform
// the server builds for - asserted below rather than assumed.
//
// This exists BECAUSE of the debounce above. Saving inside the credit lost
// nothing on a restart; deferring it by up to five seconds would - and
// `systemctl restart` sends SIGTERM, so those are exactly the seconds an
// operator would lose, on purpose, every deploy.
static_assert(std::atomic<bool>::is_always_lock_free,
              "the shutdown flag is written from a signal handler, which may only "
              "touch a lock-free atomic");
std::atomic<bool> g_shutdown{false};
extern "C" void OnTerminate(int) { g_shutdown.store(true, std::memory_order_relaxed); }
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
//MARK: UDP handshake cookie (E1)
// -------------------------------------------------------------------------
// Return-routability check for UDP. A source address in a datagram is a claim,
// not a fact - anyone can put yours in a packet they send. Before E1, a ~60 byte
// `hello` from any address at all was answered immediately with a welcome of up
// to ~3 KB, which makes this server a ~50x reflector: spoof the victim's address,
// send hellos, and we do the flooding on the attacker's behalf.
//
// The fix is the one QUIC and DTLS use. An unknown endpoint's hello gets back
// only a cookie; the client echoes it in its next hello; we recompute the cookie
// from the address we ACTUALLY OBSERVE and only then hand out a slot and a
// welcome. A spoofer never sees the cookie (it goes to the address they forged),
// so the handshake stops dead at a reply smaller than the packet that triggered
// it - there is no amplification left to sell.
//
// STATELESS is the point, not an optimisation. The cookie is derived, never
// stored: a flood of spoofed hellos costs one HMAC and one small send each and
// allocates nothing, so closing the amplification hole cannot be turned around
// into a memory-exhaustion hole.
//
// The secret behind the HMAC is the server's, shared with D3's identity token
// when that lands - see crypto.h for why it is HMAC and not hash(secret||data),
// and why it must come from the environment rather than be minted per boot.
std::string g_identitySecret;

// Cookie lifetime. A cookie is valid for its own 30s bucket and the one before,
// so it lasts 30-60s depending on when in the bucket it was minted. Long enough
// that a client on a bad link can lose a couple of hellos and still finish the
// handshake; short enough that a cookie captured off the wire is stale before
// it is worth much (and it is bound to the capturer's address anyway).
const double COOKIE_BUCKET_SEC = 30.0;

// NowSec() is the steady clock, deliberately. A cookie only has to stay valid
// for the seconds between our reply and the client's next hello, and the steady
// clock cannot jump - a wall-clock adjustment (NTP stepping the box, or a DST
// change on a badly configured host) would silently invalidate every cookie in
// flight and strand every client mid-handshake.
static uint64_t CookieBucketNow() {
    return (uint64_t)(NowSec() / COOKIE_BUCKET_SEC);
}

// The bytes a cookie commits to: the address family, the address, the port, and
// the bucket. Everything that makes the cookie specific to one peer at one time.
// Fixed-shape and length-prefixed so no two different endpoints can produce the
// same byte string (which is the only way one peer's cookie would work for
// another).
static std::string CookieMessage(const udp::endpoint& from, uint64_t bucket) {
    std::string msg;
    msg.reserve(32);
    const auto addr = from.address();
    if (addr.is_v4()) {
        const auto b = addr.to_v4().to_bytes();
        msg += (char)4; msg += (char)b.size();
        msg.append(reinterpret_cast<const char*>(b.data()), b.size());
    } else {
        const auto b = addr.to_v6().to_bytes();
        msg += (char)6; msg += (char)b.size();
        msg.append(reinterpret_cast<const char*>(b.data()), b.size());
    }
    const uint16_t port = from.port();
    msg += (char)(uint8_t)(port & 0xff);
    msg += (char)(uint8_t)(port >> 8);
    for (int i = 0; i < 8; ++i) msg += (char)(uint8_t)(bucket >> (8 * i));
    return msg;
}

// 96 bits of truncated HMAC, as 24 hex characters.
static std::string MintCookie(const udp::endpoint& from, uint64_t bucket) {
    const std::string msg = CookieMessage(from, bucket);
    return pz::HexPrefix(pz::HmacSha256(g_identitySecret, msg.data(), msg.size()), 12);
}

// True if `offered` is a cookie we issued to THIS endpoint recently.
//
// Both comparisons run even after the first one matches. Bailing out early would
// make "right cookie, wrong bucket" measurably slower than "right cookie, right
// bucket" - a smaller leak than a byte-by-byte compare, but free to avoid.
static bool CookieOk(const std::string& offered, const udp::endpoint& from) {
    if (offered.size() != 24) return false;
    const uint64_t now = CookieBucketNow();
    bool ok = pz::ConstantTimeEqual(offered, MintCookie(from, now));
    ok |= pz::ConstantTimeEqual(offered, MintCookie(from, now - 1));
    return ok;
}

// The whole reply to an unvalidated hello: 51 bytes. Nothing else may ever be
// added to it - every byte here is amplification, and this is the ONLY thing an
// unproven address can extract.
//
// The honest arithmetic, since the plan claimed a factor below 1: a real
// client's hello is ~90 bytes, so it is below 1 for anyone actually playing.
// The smallest hello a script could hand-craft is `{"type":"hello"}` at 16
// bytes, which makes the worst case 51/16 = 3.2x on payload, or 79/44 = 1.8x
// once the 28 bytes of IP+UDP header both directions carry are counted. That is
// down from ~3 KB / 90 B = 33x today, and 1.8x is not a reflector anyone would
// bother with - at that ratio you may as well aim your uplink at the victim
// directly. Getting strictly below 1 would mean a binary challenge tag (~13
// bytes) instead of JSON; the protocol says JSON, and the gap between 1.8x and
// 0.3x is not worth a message type the browser build can never use.
static std::string buildChallenge(const std::string& cookie) {
    return std::string("{\"type\":\"challenge\",\"c\":") + js(cookie) + "}";
}

//MARK: Identity token (D3)
// -------------------------------------------------------------------------
// The cookie above proves an ADDRESS is real, for the length of one handshake.
// This proves a CLIENT is the same one as last time, for as long as it keeps the
// token - which is what anything outliving a match needs, and what `clientId`
// cannot give (the client mints that itself, in a file the player owns).
//
// Same secret, same primitive, no user table: the server signs a random id and
// checks its own signature when it comes back. See server/identity.h for the
// scheme and for what it deliberately does not prove.
//
// Two different strings, and keeping them apart matters: `id` is what the server
// keys on and may log, `issue` is a bearer credential that goes to exactly one
// client and nowhere else. A helper returning "the identity" would eventually
// have somebody write the token into the score file.
struct Identity {
    std::string id;      // proved identity, or empty if identity is off entirely
    std::string issue;   // a token to hand this client, or empty if theirs is fine
};

// The whole policy in one place, used by both transports: verify what they
// presented, and if there is nothing usable, mint. It NEVER fails a connection -
// nothing, something we did not sign, and something signed before the operator
// rotated the secret are all the same situation from here, and all of them mean
// "issue a new one", not "refuse the join".
static Identity EstablishIdentity(const std::string& presented) {
    Identity out;
    if (ident::Verify(g_identitySecret, presented, out.id)) return out;

    const std::string fresh = ident::Mint(g_identitySecret);
    // No secret configured at all: identity is simply off, and everything
    // downstream must already cope with an empty id (it is what every client
    // looked like before D3).
    if (fresh.empty() || !ident::Verify(g_identitySecret, fresh, out.id)) return Identity{};
    out.issue = fresh;
    return out;
}

// Handed to a client that needs a new one. The client stores it in its profile
// and presents it from then on.
static std::string buildIdentity(const std::string& token) {
    return std::string("{\"type\":\"identity\",\"tok\":") + js(token) + "}";
}

// Bytes actually put on a socket, for A4's egress budget. Process-wide: the
// transfer quota is a property of the box, not of any one match.
EgressCounters g_egress;

// Seconds since boot, ticked by the driver loop. Reported by the heartbeat and
// by GET /status, which is the cheapest way to answer "did it restart?".
std::atomic<int> g_uptimeSeconds{0};

//MARK: Connection routing
// Which match each connection is in, and how a UDP datagram finds its connection.
// Both are server-wide: there is one UDP socket, and a packet has to be routed to
// a match BEFORE any match lock is taken.
//
// LOCK ORDER: g_connMutex > match.gameMutex > match.clientMutex > udpSendMutex.
//
// The sim thread does not take g_connMutex on its per-tick path - a match's tick
// touches only its own clients map, so routing can never convoy behind a
// simulation. The once-a-second reaper in the driver loop does take it, to move a
// destroyed room's clients home; that is outside every match lock, so it obeys the
// order above.
std::map<uint64_t, std::string> g_connMatch;   // connId -> match code ("" = unseated)
std::map<udp::endpoint, uint64_t> g_udpIndex;  // UDP source -> connId
std::mutex g_connMutex;

// Connections that are connected but hold no player slot (E2).
//
// Before E2 there was no such thing: a client that arrived when every slot was
// taken got a "full" packet and had its socket dropped, which over UDP left it
// re-helloing forever behind "MATCH IN PROGRESS - WAITING FOR A SLOT...". Being
// hung up on is the worst possible answer to "this room is full", because the
// one thing you want next is the list of rooms that are not.
//
// So fullness no longer ends a connection. It parks it here: still connected,
// still able to browse and join, and its `hello` becomes a retry for a seat. A
// connection is therefore in EXACTLY ONE of two places - some match's `clients`
// map, or this one - and every path that moves a connection has to preserve
// that. (Registry::Reap never destroys a room that still has clients, so a room
// vanishing cannot strand one here by accident.)
//
// Guarded by g_connMutex: it is connection routing, not match state.
std::map<uint64_t, ConnectedClient> g_unseated;

// Monotonic connection id. Process-level, not per-match: an id must stay unique
// across every match so a stale packet can never be mistaken for a live client.
std::atomic<uint64_t> nextConnId{1};

// The one server UDP socket, set in main() once the io_context exists. Used by
// SendToClient (sim thread) and the UDP receive handler (io threads).
udp::socket*  g_udp = nullptr;
// Serializes concurrent send_to on g_udp (Asio sockets aren't safe for
// concurrent ops on one object). Always innermost if nested under clientMutex.
std::mutex    udpSendMutex;

// Every room in the process. Still only one is ever created here - reaching it
// by code is A3 - but the plumbing is real, so A3 only has to swap a fixed
// lookup for a per-connection one instead of introducing the whole path at once.
MatchRegistry g_registry{MATCH_MAX_CONCURRENT};

// How many matches may be LIVE at once (E2). Set once in main() from
// PLATFORMZ_MAX_ACTIVE, then only read. See MATCH_MAX_ACTIVE_DEFAULT in
// constants.h for why the default makes this a no-op.
int g_maxActiveMatches = MATCH_MAX_ACTIVE_DEFAULT;

// True if `self` may begin a match right now. Counts the rooms that are already
// live, ignoring `self` so a restart of a match that is itself counted as active
// can't be blocked by its own reflection.
//
// Only ever called when a start is actually pending, which is rare - it takes
// the registry mutex, and doing that 60 times a second per room would put the
// sim thread in the way of every io thread for no reason.
static bool ActiveMatchBudgetAllows(const Match* self) {
    if (g_maxActiveMatches >= MATCH_MAX_CONCURRENT) return true;  // cap off
    int live = 0;
    for (const auto& m : g_registry.All()) {
        if (m.get() == self) continue;
        const Phase ph = m->gamePhase.load();
        if (ph == Phase::COUNTDOWN || ph == Phase::PLAYING) live++;
    }
    return live < g_maxActiveMatches;
}

// The LANDING room: where a connection goes when it names no room, where `leave`
// returns you, and the fallback when the room you asked for is gone or full.
//
// It is the FIRST official preset room (set in main, after the boot loop creates
// them), pinned like all of them so it is never reaped. It used to be a separate
// CUSTOM room named PLATFORMZ, from before the client could pick a room at all.
// Being official, it has no host and nobody can press START in it - it starts
// itself once enough humans are present.
std::string            g_defaultCode;
std::shared_ptr<Match> g_defaultMatch;

// Forward decls: Session::Read and the UDP handler both dispatch through these,
// but their bodies need Session complete (SendToClient calls Session::Send), so
// the definitions live just after the Session class.
static void SendToClient(const ConnectedClient& c, const std::string& msg);
static void HandleClientMessage(uint64_t connId, const std::string& msg);
// Which match a connection is bound to; defined with the routing helpers below,
// but Session::Read needs it to clean up the right room on disconnect.
static std::shared_ptr<Match> MatchForConn(uint64_t connId);
// Seating and forgetting, both defined with the routing helpers below. Session
// needs them at connect and at disconnect, and both must behave identically for
// the two transports - which is the whole reason they are functions and not
// inline code in each listener.
static void SeatOrPark(uint64_t connId, const ConnectedClient& rec,
                       const std::string& wantCode);
static void ForgetConn(uint64_t connId);

// Lowest player slot not owned by a connected client, or -1 if the server is
// full. Caller MUST hold gameMutex (reads players) AND clientMutex (reads clients).
//
// Two passes, not one. A slot whose body is still being held open for a
// reconnecting player (see HandleMidMatchLeavers) is skipped on the first pass,
// so a newcomer arriving during someone's 15-second grace takes an untouched
// slot instead of walking into their body and resetting it. The second pass
// gives those slots up anyway when nothing else is free: refusing a player
// entry to protect a leaver who may never return is the worse trade, and the
// leaver still gets a fresh slot if they come back.
int Match::ClaimFreeSlot() {
    auto& players = gameSpace.getPlayers();
    SlotMask claimed = 0;
    for (auto& [cid, c] : clients) SlotAdd(claimed, c.playerId);
    for (int i = 0; i < (int)players.size(); i++)
        if (!SlotSet(claimed, i) && players[i].leaveGraceSec < 0.0f) return i;
    for (int i = 0; i < (int)players.size(); i++)
        if (!SlotSet(claimed, i)) return i;
    return -1;
}

// The slot this clientId is entitled to resume, or -1. Every condition here is
// load-bearing:
//   non-empty id   an older client sends none; "" must never match the "" left
//                  in an untouched slotOwner entry, or the first joiner to a
//                  fresh match would "resume" slot 0.
//   vacant         somebody else is sitting there now; they win, we get a new slot.
//   grace running  leaveGraceSec > 0 means HandleMidMatchLeavers is holding this
//                  body open right now. Below zero it either expired (the body is
//                  eliminated) or was never armed (LOBBY, where there is no body
//                  to resume and slots get compacted anyway).
//   alive          belt and braces with the above: never seat someone into a corpse.
// Caller MUST hold gameMutex AND clientMutex.
int Match::HeldSlotFor(const std::string& clientId) {
    if (clientId.empty()) return -1;
    auto& players = gameSpace.getPlayers();
    SlotMask taken = 0;
    for (auto& [cid, c] : clients) SlotAdd(taken, c.playerId);
    for (int i = 0; i < (int)players.size() && i < GAMESPACE_NUMBER_OF_PLAYERS; ++i) {
        if (SlotSet(taken, i))            continue;
        if (slotOwner[i] != clientId)     continue;
        if (players[i].leaveGraceSec <= 0.0f) continue;
        if (!players[i].isAlive)          continue;
        return i;
    }
    return -1;
}

// Drop a stale connection that is already holding this clientId, so the player
// returning under a new network identity can have their own slot back.
//
// Needed because the two clocks disagree. A client gives up after ~3 s of
// silence and re-runs the handshake; the server does not free a quiet UDP slot
// for UDP_CLIENT_TIMEOUT (10 s mid-match). In the window between, a laptop that
// woke with a new NAT mapping arrives as a stranger while its old endpoint still
// owns the slot - so the resume finds the slot occupied, hands out a fresh one,
// and the body it was coming back for drifts off and dies. Exactly the case this
// feature exists for.
//
// The silence guard is what keeps this from being a footgun: two clients run
// from one machine share a profile, and therefore a clientId (LAN testing does
// this routinely). A twin that is actively sending is a real second player and
// is left alone; only one that has already gone quiet is treated as the same
// player's abandoned connection.
//
// WS records are never superseded - TCP delivers a real disconnect, so a live WS
// connection with this id is genuinely someone else at the keyboard.
//
// Caller MUST hold clientMutex.
void Match::SupersedeStaleTwin(const std::string& clientId,
                               const boost::asio::ip::udp::endpoint& newEndpoint) {
    if (clientId.empty()) return;
    const double now = NowSec();
    for (auto it = clients.begin(); it != clients.end(); ) {
        const ConnectedClient& c = it->second;
        const bool twin = c.transport == Transport::UDP
                       && c.clientId == clientId
                       && c.udpEndpoint != newEndpoint
                       && (now - c.lastSeenSec) > UDP_TWIN_SUPERSEDE_SEC;
        if (!twin) { ++it; continue; }
        std::cout << "UDP client " << c.udpEndpoint
                  << " superseded by the same client id on a new endpoint"
                  << " (slot " << c.playerId << ")\n";
        { std::lock_guard<std::mutex> cl(g_connMutex);
          g_udpIndex.erase(c.udpEndpoint);
          g_connMatch.erase(it->first); }
        it = clients.erase(it);
    }
    connectedCount.store((int)clients.size());
}

// Seat a human: resume the slot they were holding, or take a fresh one.
//
// The resume path deliberately does NOTHING to the body beyond cancelling the
// eviction countdown. Position, velocity, health, ammo and score are exactly as
// they were left, which is the entire feature - routing a reconnect through
// TakeOverSlot would hand the player their own body wiped clean and lose the
// score they are coming back to defend.
//
// Caller MUST hold gameMutex AND clientMutex.
Match::Seat Match::SeatPlayer(const std::string& clientId, const std::string& name) {
    Seat seat;
    seat.slot = HeldSlotFor(clientId);
    if (seat.slot >= 0) {
        seat.resumed = true;
        auto& p = gameSpace.getPlayers()[seat.slot];
        p.leaveGraceSec = -1.0f;    // back before the countdown ran out
        p.isBot         = false;    // never botified mid-match, but say so explicitly
        p.isVacant      = false;    // likewise: HeldSlotFor requires isAlive, so a
                                    // vacant slot can never be resumed - stated anyway
        Message msg(MSG_TYPE_REJOINED_GAME, p.name, p.name, p.id, p.id);
        gameSpace.emitMessage(msg);
    } else {
        seat.slot = ClaimFreeSlot();
        if (seat.slot < 0) return seat;
        TakeOverSlot(seat.slot, name);
    }
    if (seat.slot >= 0 && seat.slot < GAMESPACE_NUMBER_OF_PLAYERS)
        slotOwner[seat.slot] = clientId;
    return seat;
}

// UDP has no disconnect event, so a client that quit just goes quiet. Free any
// UDP slot whose last packet is older than the timeout. Called both on the
// periodic per-tick sweep AND right before a new connection claims a slot
// (Session::Accept / RegisterPeer) - the latter closes a race where a fresh
// hello/accept could otherwise land ahead of that tick's sweep and get pushed
// to a higher slot than a just-expired one it should have reclaimed (e.g. a
// client reconnecting under a new source endpoint after its old one goes
// stale). Uses the shorter UDP_CLIENT_TIMEOUT_LOBBY while nobody's mid-action;
// the longer UDP_CLIENT_TIMEOUT otherwise, to survive a mid-match frame hitch.
// Caller MUST hold clientMutex.
void Match::ReapIdleUdpClients() {
    double now     = NowSec();
    double timeout = gamePhase.load() == Phase::LOBBY ? UDP_CLIENT_TIMEOUT_LOBBY
                                                        : UDP_CLIENT_TIMEOUT;
    for (auto it = clients.begin(); it != clients.end(); ) {
        if (it->second.transport == Transport::UDP &&
            now - it->second.lastSeenSec > timeout) {
            std::cout << "UDP player " << it->second.playerId
                      << " timed out. Active: " << (clients.size() - 1) << "\n";
            { std::lock_guard<std::mutex> cl(g_connMutex); g_udpIndex.erase(it->second.udpEndpoint); }
            it = clients.erase(it);
            connectedCount.store((int)clients.size());
        } else {
            ++it;
        }
    }
}

// Compact every connected client into the lowest slots, in their current
// slot order (order-preserving, so the lowest-slot client - the host - stays
// the host). Closes the hole a leaver left behind: the next-lowest client
// takes over the vacated slot instead of it just sitting empty/bot-ified
// while they stay parked higher up. Returns the connIds whose playerId
// actually changed, so the caller can push each of them a fresh welcome -
// a client only learns "which slot is mine" from a welcome message, so
// changing playerId without one would leave it rendering/inputting as its
// old slot. Caller MUST hold clientMutex.
std::vector<uint64_t> Match::CompactConnectedSlots() {
    std::vector<uint64_t> changed;
    std::vector<std::pair<int, uint64_t>> order; // (slot, connId)
    for (auto& [cid, c] : clients) order.push_back({c.playerId, cid});
    std::sort(order.begin(), order.end());
    for (int i = 0; i < (int)order.size(); ++i) {
        auto& c = clients[order[i].second];
        if (c.playerId != i) {
            std::cout << "Slot compact: player " << c.playerId
                      << " -> " << i << " (" << c.name << ")\n";
            changed.push_back(order[i].second);
        }
        c.playerId  = i;
        c.nameDirty = true;
    }
    return changed;
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
//
// `allowBotify` is true in LOBBY/COUNTDOWN/GAMEOVER: the match roster is only
// truly locked while PLAYING. Mid-match (PLAYING) a claimed slot still flips
// bot->human (a joiner takes over the body), but a leaver's slot is NOT given
// to a bot - the body stays open, drifting, keeping its name/score/health, so
// the player can reconnect and resume it (ClaimFreeSlot hands back the lowest
// free slot, which is theirs - humans are compacted below the bots). Once
// GAMEOVER, though, the client is already back on the roster-showing screen
// (see returnToTitle in main.cpp), so a leaver's slot is relabeled right away
// instead of sitting on their stale name until the next match start.
//
// Only the first `maxBots` unclaimed slots are filled (MatchPreset, options.h);
// the rest are VACATED - marked isVacant, killed, and handed back their default
// name and color. A vacant slot is a slot with no body at all: nothing to shoot,
// nothing to drive, not counted for last-man-standing, and still joinable. It is
// NOT the same thing as a mid-match leaver's open body, which keeps drifting and
// stays killable; HandleMidMatchLeavers tells them apart by isAlive.
//
// Bots fill from the LOWEST unclaimed slot upward, so vacancies collect at the
// top. That is not arbitrary: humans are compacted into the lowest slots
// (CompactConnectedSlots), and setPlayerCount pops from the TAIL, so a match
// start that shrinks the roster discards empty slots first and never disturbs
// the bot set. Bot names are indexed by SLOT, not by bot ordinal, so a slot's
// name holds steady for as long as it is a bot however many humans come and go.
void Match::refreshBotSlots(SlotMask claimed, bool allowBotify) {
    auto& players = gameSpace.getPlayers();
    const int maxBots = pendingMaxBots.load();
    int botsSoFar = 0;
    for (int i = 0; i < (int)players.size(); ++i) {
        bool bot = !SlotSet(claimed, i);
        if (!bot) { players[i].isBot = false; players[i].isVacant = false; continue; }
        if (!allowBotify) continue; // mid-match leaver: leave the slot open
        if (botsSoFar >= maxBots) {
            // Over the cap: no body here. Clearing the name and color matters -
            // without it a joiner who has not sent their name message yet
            // renders for a tick as a magenta ghost called GEOFF. Idempotent, so
            // running it sixty times a second costs nothing and never thrashes.
            Player& p = players[i];
            p.isBot    = false;
            p.isVacant = true;
            p.isAlive  = false;
            p.velocity = {0, 0, 0};
            p.name     = "PLAYER " + std::to_string(i + 1);
            assignPlayerColor(p, i);
            continue;
        }
        ++botsSoFar;
        players[i].isBot    = true;
        players[i].isVacant = false;
        // Through THIS MATCH's shuffled order, so a room does not field the same
        // lineup in the same slots every time (#102). Local mode has always done
        // this; the naming moved server-side and the shuffle did not come with it.
        // Guarded modulo so a bot at slot 0 (all humans gone) still names cleanly.
        const int slotIdx = ((i - 1) % BOT_NAME_COUNT + BOT_NAME_COUNT) % BOT_NAME_COUNT;
        // Defensive: an order that is somehow the wrong length falls back to the
        // slot itself rather than reading off the end. Never happens - it is
        // built once at construction and only ever replaced wholesale - but this
        // runs every tick for every slot in every room, and a bad index here
        // would be a crash rather than a cosmetic bug.
        const int nameIdx = (slotIdx < (int)botNameOrder.size())
                          ? botNameOrder[slotIdx] : slotIdx;
        players[i].name          = BOT_NAME_STRINGS[nameIdx];
        players[i].color_outline = BOT_OUTLINE_COLOR;
        players[i].color_fill    = BOT_FILL_COLOR;
    }
}

// Companion to refreshBotSlots for the case it deliberately leaves alone: a
// mid-match slot whose client left while PLAYING (so `!allowBotify` - see
// refreshBotSlots' comment). That body stays open for a reconnect, but if
// nobody reconnects within MID_MATCH_LEAVE_GRACE_SEC, eliminate it so it
// stops being a pointless, uncontrolled target: same direct-elimination
// pattern as the "lost in space" path (gamespace.h) - no takeDamage(), no
// damage source - plus a kill-feed message everyone sees. Caller MUST hold
// gameMutex (reads/writes players, emits a message) AND clientMutex
// (indirectly, via the already-gathered `claimed` set - this helper itself
// never touches `clients`). No-ops entirely while `allowBotify` (LOBBY/
// COUNTDOWN/GAMEOVER): those slots become bots instead via refreshBotSlots.
void Match::HandleMidMatchLeavers(SlotMask claimed, bool allowBotify, float dt) {
    if (allowBotify) return;
    auto& players = gameSpace.getPlayers();
    for (int i = 0; i < (int)players.size(); ++i) {
        Player& p = players[i];
        if (SlotSet(claimed, i)) {
            p.leaveGraceSec = -1.0f; // reclaimed (reconnect or new joiner) - cancel any countdown
            continue;
        }
        if (p.isBot) continue;      // a real bot slot, never had a human - not a leaver
        // A VACANT slot (over the preset's maxBots) is also unclaimed and also
        // not a bot, but it is not a leaver either - it never had a body. It
        // falls into the !isAlive branch below, which clears the countdown and
        // moves on, which is exactly right: nothing to grace, nothing to
        // eliminate, no "left the game" message.
        if (!p.isAlive) {
            // Dead, and nobody is sitting in it: there is nothing left to come
            // back to, so stop reserving the slot. Clearing the countdown here
            // is what keeps this function and HeldSlotFor agreeing - HeldSlotFor
            // already refuses to resume a body that is not alive, but
            // ClaimFreeSlot's first pass reads leaveGraceSec, and those two
            // disagreed in exactly one case: an unattended body DESTROYED while
            // its grace was still running. That path leaves here through the
            // `continue` below without ever reaching the decrement, so the
            // countdown froze at whatever positive value it held - forever - and
            // the first pass treated a dead, ownerless slot as reserved for the
            // rest of the match. A latecomer then took a higher slot while slot
            // 0 sat empty, which is an intermittent bug precisely because it
            // depends on whether an asteroid happens to finish the body off
            // inside those 15 seconds. (Pre-existing; found by probe_host
            // failing about two runs in five.)
            p.leaveGraceSec = -1.0f;
            continue;
        }
        if (p.leaveGraceSec < 0.0f) {
            p.leaveGraceSec = MID_MATCH_LEAVE_GRACE_SEC; // just noticed vacant - arm the countdown
            continue;
        }
        p.leaveGraceSec -= dt;
        if (p.leaveGraceSec <= 0.0f) {
            p.isAlive = false;
            Message msg(MSG_TYPE_LEFT_GAME, p.name, p.name, p.id, p.id);
            gameSpace.emitMessage(msg);
        }
    }
}

// Gather the claimed-slot set. Caller must hold clientMutex.
SlotMask Match::gatherClaimedSlots() {
    SlotMask claimed = 0;
    for (auto& [cid, c] : clients) SlotAdd(claimed, c.playerId);
    return claimed;
}

// Is this connection the lobby "host"? The host is the client owning the lowest
// player slot (playerId) among all connected clients - the same "player 1 = lowest
// connected slot" rule the client uses to gate its lobby UI. Only the host may
// start a match or change OPTIONS; this is the authoritative backstop behind that
// UI gate. Locks clientMutex itself; returns false for an unknown conn or no
// clients. Self-contained (never touches gameMutex), so no lock-ordering risk.
// Who hosts this room right now, migrating if the host on file has left.
//
// STICKY, which is the whole point: once a host is set it stays put even when a
// lower slot opens up beside them. The old rule recomputed "lowest connected
// slot" every time and so silently handed the room to whoever landed in slot 0.
//
// Migration target is the lowest remaining slot - today's rule, but applied ONCE
// on departure instead of continuously.
uint64_t Match::ResolveHostLocked() {
    // An OFFICIAL room has no host at all. Returning 0 here keeps isHostConn and
    // optionsLocked from disagreeing: nobody hosts it, and nobody may retune or
    // start it. The two used to be able to contradict each other.
    if (optionsLocked) { hostConn = 0; return 0; }

    if (hostConn != 0 && clients.count(hostConn)) return hostConn;

    uint64_t best = 0;
    int bestSlot = INT_MAX;
    for (const auto& [cid, c] : clients) {
        if (c.playerId >= 0 && c.playerId < bestSlot) { bestSlot = c.playerId; best = cid; }
    }
    hostConn = best;   // 0 when the room is empty; the next arrival takes it
    return hostConn;
}

int Match::HostSlotLocked() {
    const uint64_t h = ResolveHostLocked();
    if (h == 0) return -1;
    auto it = clients.find(h);
    return it == clients.end() ? -1 : it->second.playerId;
}

bool Match::isHostConn(uint64_t connId) {
    std::lock_guard<std::mutex> lock(clientMutex);
    return connId != 0 && ResolveHostLocked() == connId;
}

//MARK: Auto-start
// A public room has no host to press START, so it starts itself. Arms once this
// room's minHumansToStart humans are present and disarms if it drops back below
// that, so a room that half-fills and empties doesn't launch at one player. The
// threshold is per-PRESET (MatchPreset, options.h) and defaults to
// PUBLIC_MIN_PLAYERS, so a preset that says nothing behaves as it always did.
// LOBBY only - once COUNTDOWN begins the normal path owns it. Caller holds
// gameMutex.
void Match::ServiceAutoStart(Clock::time_point now) {
    if (!autoStart) return;
    if (gamePhase.load() != Phase::LOBBY) { autoStartArmed = false; return; }

    const int live = connectedCount.load();
    if (live < pendingMinHumans.load()) {
        if (autoStartArmed) std::cout << "Auto-start disarmed (players " << live << ")\n";
        autoStartArmed = false;
        countdownRemaining = 0.0f;   // the room emptied back below the threshold
        return;
    }
    if (!autoStartArmed) {
        autoStartArmed = true;
        autoStartAt = now + std::chrono::duration_cast<Clock::duration>(
                                std::chrono::duration<double>(PUBLIC_AUTOSTART_SECONDS));
        countdownRemaining = (float)PUBLIC_AUTOSTART_SECONDS;  // publish it on the arming tick, not one later
        std::cout << "Auto-start armed: " << live << " players, "
                  << (int)PUBLIC_AUTOSTART_SECONDS << "s\n";
        return;
    }
    // Let the room SEE it coming. countdownRemaining is otherwise only written
    // for the pre-match COUNTDOWN phase and is 0 throughout LOBBY, and the client
    // reads it only on its countdown screen - so reusing it here cost no new
    // state field and, more to the point, no STATE_BIN_VERSION bump. A nonzero
    // countdown in LOBBY is unambiguous: nothing else sets one.
    countdownRemaining = (float)std::max(0.0,
        std::chrono::duration<double>(autoStartAt - now).count());

    if (now >= autoStartAt) {
        autoStartArmed = false;
        countdownRemaining = 0.0f;
        startRequested = true;   // consumed by the normal start path next tick
        std::cout << "Auto-start firing\n";
    }
}

//MARK: Join in progress
// Hand a bot's slot - or an EMPTY one - to a human who has just joined a live
// match.
//
// WHAT IS INHERITED AND WHAT IS NOT, because "take over the bot" is ambiguous and
// the wrong split is either unfair or miserable:
//
//   position/velocity  INHERITED. A valid, in-world spot. Spawning fresh mid-match
//                      risks dropping someone inside a platform or on top of a
//                      firefight, and placePlayersSpread only makes sense against
//                      an empty arena.
//   health/fuel/ammo   RESET. Inheriting a bot on 5 HP means joining a match is
//                      usually instant death, which is a bad first five seconds.
//   score              RESET. You did not earn the bot's points, and the scoreboard
//                      credits at match end - inheriting would put unearned points
//                      on a permanent leaderboard.
//   alive              REVIVED. ClaimFreeSlot does not check isAlive, so without
//                      this you can join straight into a corpse and spectate a
//                      match you never played.
//   colour             RESET to this slot's human colour. Otherwise the newcomer
//                      renders in bot magenta for the rest of the match.
//   vacancy            CLEARED. A slot the preset's maxBots left empty has no
//                      body; the same reset above gives it one. Its position is
//                      the placePlayersSpread spawn point with zero velocity -
//                      a cleaner arrival than inheriting a bot mid-flight.
//
// Caller holds gameMutex. Safe in any phase: in LOBBY there is no world yet and
// this is a harmless no-op on preview data.
void Match::TakeOverSlot(int slot, const std::string& joinerName) {
    auto& players = gameSpace.getPlayers();
    if (slot < 0 || slot >= (int)players.size()) return;
    Player& p = players[slot];

    const bool wasBot    = p.isBot;
    const bool wasVacant = p.isVacant;
    p.isBot    = false;
    p.isVacant = false;
    p.isAlive  = true;
    p.health  = PLAYER_STARTING_HEALTH;
    p.fuel    = PLAYER_STARTING_FUEL;
    p.ammo    = PLAYER_STARTING_AMMO;
    p.score   = 0;
    p.leaveGraceSec      = -1.0f;   // cancel any mid-match-leaver countdown
    p.deathBurstSpawned  = false;
    p.isSpectating       = false;
    p.spectatingTimer    = p.countdownToSpectating;
    assignPlayerColor(p, slot);

    // Only announce a real mid-match takeover. A lobby join is already visible in
    // the roster, and saying it there would be noise. Filling an EMPTY slot counts
    // as one too - somebody just appeared in the arena either way, and in a
    // maxBots-capped room that is the only kind of arrival there is.
    if ((wasBot || wasVacant) && gamePhase.load() == Phase::PLAYING) {
        const std::string who = joinerName.empty() ? p.name : joinerName;
        Message msg(MSG_TYPE_JOINED_GAME, who, who, p.id, p.id);
        gameSpace.emitMessage(msg);
    }
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

// Clamp a client-supplied display name to the shared cap (constants.h) and drop
// anything outside printable ASCII. The stock client's entry field already enforces
// both, so this is the backstop against a modified client: an overlong name would
// overflow everyone else's UI, and a control character would corrupt two things
// downstream - a tab or newline breaks the scoreboard's one-line-per-entry file,
// and js() (which only escapes " and \) would emit invalid JSON for the rest.
// Strip first, then clamp, so removing characters can't leave it over the cap.
static std::string clampName(std::string name) {
    name.erase(std::remove_if(name.begin(), name.end(),
                              [](unsigned char c) { return c < 32 || c > 126; }),
               name.end());
    if (name.size() > PLAYER_NAME_MAX_CHARS) name.resize(PLAYER_NAME_MAX_CHARS);
    return name;
}

// Same scrubbing for an identifier, but NOT clampName's 32-char cap: a
// version-4 UUID is 36 characters, so clamping one as if it were a display name
// would quietly saw the last four off. It still needs a bound - this is
// untrusted input we store per slot - so 64, which leaves room for the longer
// signed token D3 will carry without revisiting this.
//
// Never rendered, only compared for equality, so the character rule is about
// keeping junk out of logs rather than about layout.
static std::string clampClientId(std::string id) {
    id.erase(std::remove_if(id.begin(), id.end(),
                            [](unsigned char c) { return c < 32 || c > 126; }),
             id.end());
    if (id.size() > CLIENT_ID_MAX_CHARS) id.resize(CLIENT_ID_MAX_CHARS);
    return id;
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
std::string Match::buildPlatformsArray() {
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
void Match::rebuildWelcomeStatic() {
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
std::string Match::buildWelcome(int playerId) {
    std::string statik;
    { std::lock_guard<std::mutex> lk(welcomeStaticMutex); statik = welcomeStatic; }
    // "m" and "k": which room this is and how it is run. See buildWelcomeBinary
    // for why the client cannot work either out for itself.
    return "{\"type\":\"welcome\",\"playerId\":" + std::to_string(playerId)
         + ",\"tick\":" + std::to_string(serverTick.load())
         + ",\"m\":" + js(matchCode)
         + ",\"k\":" + js(matchKindWire(matchKind))
         + "," + statik + "}";
}

// Binary welcome for UDP clients: the platform list can exceed the MTU as JSON,
// so pack it (netbin.h). Field order matches the client's applyBinaryWelcome.
std::string Match::buildWelcomeBinary(int playerId) {
    std::string statik;
    { std::lock_guard<std::mutex> lk(welcomeStaticMutex); statik = welcomeStaticBin; }
    std::string b;
    nb::putU8(b, nb::WELCOME_BIN_VERSION);
    nb::putI32(b, playerId);
    nb::putU32(b, serverTick.load());
    // Room identity. Neither is derivable client-side: a player may have arrived
    // by quick match or by connecting with no room named, so the code they think
    // they asked for is not authoritative - and nothing else on the wire says how
    // a room they are already inside is governed, which is what decides whether
    // they get a START button or a countdown. Inserted BEFORE the static world,
    // so the layout moved and the tag had to move with it.
    nb::putStr(b, matchCode);
    nb::putU8(b, matchKind == MatchKind::Official ? 1 : 0);
    b += statik;   // f32 half, u16 platformCount, platforms
    return b;
}

// Pick the welcome format for a client's transport (binary over UDP, JSON over
// WebSocket) - used at every welcome-send site except the WS-only connect path.
std::string Match::welcomeFor(const ConnectedClient& c) {
    return c.transport == Transport::UDP ? buildWelcomeBinary(c.playerId)
                                         : buildWelcome(c.playerId);
}

//MARK: Leaderboard packet
// The arcade board (D5): the best RUNS, not career totals. A run is one finished
// match from one player's side, so the same person can hold several rows - which
// is the point, and what makes beating your own third place a normal evening.
//
// Plain JSON on purpose - it goes out through SendToClient, which ships text over
// BOTH transports (chunking oversized UDP datagrams), and the client's applyMessage
// only treats a packet as binary when it leads with a binary tag byte. So this
// reaches WS and UDP clients alike without touching the binary welcome format.
//
// Built PER CLIENT, because `best` is that client's own best run pinned under the
// board. Cheap: eleven rows, sent on join and at match end, not per tick.
static std::string buildLeaderboard(const std::string& identity) {
    std::string s = "{\"type\":\"leaderboard\",\"lb\":[";
    {
        std::lock_guard<std::mutex> lk(scoreboardMutex);
        const auto rows = scoreboard.topRuns(g_scoresOfficialOnly, SCORES_BOARD_ROWS);
        auto row = [](const RunRow& r) {
            return "{\"n\":" + js(r.name) + ",\"s\":" + ji(r.score)
                 + ",\"b\":" + jb(r.isBot()) + "}";
        };
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) s += ",";
            s += row(rows[i]);
        }
        s += "]";

        // The pin. Omitted when they have never recorded a run, and ALSO when
        // their best is already up there - deciding that here rather than in the
        // client means "do not show it twice" has one implementation instead of
        // one per platform.
        RunRow best;
        if (scoreboard.bestRunFor(identity, g_scoresOfficialOnly, best)) {
            bool onBoard = false;
            for (const RunRow& r : rows)
                if (r.id == best.id && r.score == best.score && r.when == best.when)
                    onBoard = true;
            if (!onBoard) s += ",\"best\":" + row(best);
        }
    }
    s += "}";
    return s;
}

//MARK: State packet
// -------------------------------------------------------------------------
// State serialization - build the JSON state body sent to all clients.
// Every client sees the same world this tick - only `tick`/`lastSeq` (the tiny
// per-client header built by buildStatePacket below) actually differ, so this
// body is built exactly ONCE per tick in BroadcastState and reused for every
// connected WS client instead of being re-serialized per client.
// `connectedSlots` are the player indices currently occupied by a client; each
// player carries an "active" flag so clients can skip rendering empty slots.
// -------------------------------------------------------------------------
std::string Match::buildStateBodyJson(SlotMask connectedSlots, int hostSlot) {
    std::string s;
    s.reserve(1024);
    s += ",\"phase\":\"" + std::string(phaseString(gamePhase.load())) + "\"";
    s += ",\"countdown\":" + jf(countdownRemaining.load()); // seconds left in the pre-match countdown (0 unless COUNTDOWN)
    s += ",\"ep\":" + std::to_string(matchEpoch.load()); // match epoch; clients echo it in their input packets

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
        s += ",\"stmr\":"   + jf(p.spectatingTimer); // post-death spectate countdown (drives the client's greyscale ramp)
        s += ",\"bot\":"    + jb(p.isBot); // server-owned bot flag (set for unoccupied slots during a match)
        s += ",\"flash\":"  + jf(p.flashTimer); // damage-flash, so the client can glow a hit body
        s += ",\"oob\":"    + jb(p.isOutOfBounds);   // server-owned: outside the boundary, elimination pending
        s += ",\"oobt\":"   + jf(p.outOfBoundsTimer); // seconds left before being lost in space (drives the HUD countdown)
        // A slot is shown if a human occupies it, a bot drives it, or - once a
        // match is underway (roster final) - whenever it is not VACANT, so a
        // mid-match leaver's open body stays visible/killable instead of going
        // invisible, while a slot the preset's maxBots left empty never had a
        // body to show. This flag is also how the client learns a slot is empty:
        // isVacant itself never crosses the wire.
        s += ",\"active\":" + jb(SlotSet(connectedSlots, i) || p.isBot
                                 || (gamePhase.load() != Phase::LOBBY && !p.isVacant));
        s += ",\"score\":"  + ji(p.score); // server-owned score (credited in collisions)
        // Who runs this room. Server-owned, because the client can no longer work
        // it out: host is the CREATOR now, not whoever holds the lowest slot, and
        // an official room has no host at all (so no slot carries this).
        s += ",\"host\":"   + jb(i == hostSlot);
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
    s += ",\"maxbots\":"  + ji(pendingMaxBots.load());
    s += ",\"minhumans\":" + ji(pendingMinHumans.load());
    s += ",\"diff\":"     + jf(pendingDiff.load());
    s += ",\"welast\":"   + jf(pendingWallElast.load());
    s += ",\"pelast\":"   + jf(pendingPlatElast.load());
    s += ",\"boost\":"    + jf(pendingBoost.load());
    s += ",\"rspeed\":"   + jf(pendingRocketSpeed.load());
    s += ",\"xradius\":"  + jf(pendingXRadius.load());
    s += ",\"jthrust\":"  + jf(pendingJThrust.load());
    s += ",\"fburn\":"    + ji(pendingFuelBurn.load());
    s += ",\"fregen\":"   + ji(pendingFuelRegen.load());
    s += ",\"walls\":"    + jb(pendingWallsEnabled.load());
    s += ",\"phys\":"     + jb(pendingRocketsPhysics.load());
    s += ",\"ff\":"       + jb(pendingFriendlyFire.load());
    s += ",\"coast\":"    + jb(pendingCoastMode.load());
    s += ",\"map\":"      + js(MapSizeName(pendingMap.load())); // which arena, visible in the lobby before start
    s += "}";

    s += "}";
    return s;
}

// Cheap per-client wrapper: prepends the tiny header (type/tick/seq) that
// actually varies per client onto the shared `body` built once per tick by
// buildStateBodyJson above.
std::string Match::buildStatePacket(uint32_t tick, uint32_t lastSeq,
                                    const std::string& body) {
    std::string s;
    s.reserve(32 + body.size());
    s += "{\"type\":\"state\"";
    s += ",\"tick\":" + ju(tick);
    s += ",\"seq\":"  + ju(lastSeq);
    s += body;
    return s;
}

//MARK: State packet (binary)
// -------------------------------------------------------------------------
// The same state as buildStateBodyJson, packed as little-endian binary (netbin.h)
// for UDP clients so it fits one datagram (~4x smaller than the JSON). Field
// ORDER here must match the client's applyBinaryState() reader in wire.h.
// Unused-on-the-wire fields are dropped: asteroid/rocket `dead` (the server never
// sends dead=true - destroyed objects just vanish from the set).
//
// Same once-per-tick split as buildStateBodyJson: this body excludes the
// header (tag/tick/lastSeq), which buildStateBinary below prepends per client.
// -------------------------------------------------------------------------
std::string Match::buildStateBodyBinary(SlotMask connectedSlots, int hostSlot) {
    std::string b;
    b.reserve(768);
    nb::putU8(b, (uint8_t)gamePhase.load()); // Phase enum: 0 lobby,1 countdown,2 playing,3 gameover
    nb::putF32(b, countdownRemaining.load());
    nb::putU32(b, matchEpoch.load()); // match epoch; clients echo it in their input packets

    // Options (match-wide), same values buildStatePacket puts in "opt". Order
    // must match applyBinaryState() in wire.h exactly.
    nb::putU8(b, (uint8_t)pendingPlayers.load());
    // The two sliderless rules. Two bytes, which together with the roster byte
    // above are what cost STATE_BIN_VERSION 0x09 -> 0x0B: the flags byte had
    // only two free bits and these need four each.
    nb::putU8(b, (uint8_t)pendingMaxBots.load());
    nb::putU8(b, (uint8_t)pendingMinHumans.load());
    nb::putF32(b, pendingDiff.load());
    nb::putF32(b, pendingWallElast.load());
    nb::putF32(b, pendingPlatElast.load());
    nb::putF32(b, pendingBoost.load());
    nb::putF32(b, pendingRocketSpeed.load());
    nb::putF32(b, pendingXRadius.load());
    nb::putF32(b, pendingJThrust.load());
    nb::putU8(b, (uint8_t)pendingFuelBurn.load());
    nb::putU8(b, (uint8_t)pendingFuelRegen.load());
    // Bits 16/32 carry the map index. Four toggles left the top nibble free, so
    // the arena reaches every client in the lobby without growing the packet or
    // spending a STATE_BIN_VERSION bump.
    nb::putU8(b, (uint8_t)((pendingWallsEnabled.load() ? 1 : 0)
                          | (pendingRocketsPhysics.load() ? 2 : 0)
                          | (pendingFriendlyFire.load() ? 4 : 0)
                          | (pendingCoastMode.load() ? 8 : 0)
                          | ((pendingMap.load() & 0x3) << 4)));

    // Players (fixed roster; u8 count is plenty).
    auto& players = gameSpace.getPlayers();
    nb::putU8(b, (uint8_t)players.size());
    for (int i = 0; i < (int)players.size(); i++) {
        const Player& p = players[i];
        nb::putU32(b, p.id);
        nb::putQPos(b, p.position.x); nb::putQPos(b, p.position.y); nb::putQPos(b, p.position.z);
        nb::putQVel(b, p.velocity.x); nb::putQVel(b, p.velocity.y); nb::putQVel(b, p.velocity.z);
        nb::putQAngle(b, p.yaw); nb::putQAngle(b, p.pitch);
        nb::putU8(b, (uint8_t)p.health);
        nb::putQFrac(b, p.fuel, PLAYER_MAX_FUEL);
        nb::putU8(b, (uint8_t)p.ammo);
        nb::putQFrac(b, p.flashTimer, Player::flash_duration);
        nb::putQFrac(b, p.spectatingTimer, p.countdownToSpectating);
        nb::putU16(b, (uint16_t)p.score);
        // Same rule as the JSON builder: in-match slots stay visible even when
        // their human left (open body awaiting a reconnect), but a VACANT slot
        // (over the preset's maxBots) never had a body and stays hidden.
        bool active = SlotSet(connectedSlots, i) || p.isBot
                      || (gamePhase.load() != Phase::LOBBY && !p.isVacant);
        // Bit 32 is the host flag. Bits 32/64/128 were free, so this needed no
        // layout change and no STATE_BIN_VERSION bump - a client built before it
        // masks the bits it knows and ignores this one.
        nb::putU8(b, (uint8_t)((p.isAlive ? 1 : 0) | (p.isBot ? 2 : 0) | (active ? 4 : 0) | (p.isSpectating ? 8 : 0)
                              | (p.isOutOfBounds ? 16 : 0) | (i == hostSlot ? 32 : 0)));
        // Out-of-bounds countdown. Player::updatePos (which owns this timer)
        // only runs server-side, so without this the client can't show how long
        // is left before elimination - see the HUD block in main.cpp.
        nb::putQFrac(b, p.outOfBoundsTimer, OUT_OF_BOUNDS_TIMER);
        nb::putStr(b, p.name);
    }

    // Asteroids.
    auto& asteroids = gameSpace.getAsteroids();
    nb::putU16(b, (uint16_t)asteroids.size());
    for (const Asteroid& a : asteroids) {
        nb::putU32(b, a.id);
        nb::putQPos(b, a.position.x); nb::putQPos(b, a.position.y); nb::putQPos(b, a.position.z);
        nb::putQVel(b, a.velocity.x); nb::putQVel(b, a.velocity.y); nb::putQVel(b, a.velocity.z);
        nb::putQFrac(b, a.size, ASTEROID_SIZE_ENCODE_MAX);
        nb::putU8(b, (uint8_t)a.health);
        nb::putQFrac(b, a.flashTimer, ASTEROID_FLASH_DURATION);
    }

    // Rockets.
    auto& rockets = gameSpace.getRockets();
    nb::putU16(b, (uint16_t)rockets.size());
    for (const Rocket& r : rockets) {
        nb::putU32(b, r.id);
        nb::putQPos(b, r.position.x); nb::putQPos(b, r.position.y); nb::putQPos(b, r.position.z);
        nb::putQVel(b, r.velocity.x); nb::putQVel(b, r.velocity.y); nb::putQVel(b, r.velocity.z);
    }

    // Explosions (ephemeral, no id).
    auto& explosions = gameSpace.getExplosions();
    nb::putU16(b, (uint16_t)explosions.size());
    for (const Explosion& e : explosions) {
        nb::putQPos(b, e.position.x); nb::putQPos(b, e.position.y); nb::putQPos(b, e.position.z);
        nb::putQFrac(b, e.radius, EXPLOSION_RADIUS_ENCODE_MAX);
        nb::putU8(b, e.isActive ? 1 : 0);
    }

    // Audio events (one-shots this tick).
    auto& audio = gameSpace.getAudioEvents();
    nb::putU8(b, (uint8_t)audio.size());
    for (const auto& ev : audio) {
        nb::putU8(b, (uint8_t)ev.fx);
        nb::putU32(b, ev.owner);
        nb::putQPos(b, ev.pos.x); nb::putQPos(b, ev.pos.y); nb::putQPos(b, ev.pos.z);
        nb::putQFrac(b, ev.volumeScale, 1.0f);
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

// Cheap per-client wrapper: prepends the tiny header (tag/tick/lastSeq) that
// actually varies per client onto the shared `body` built once per tick by
// buildStateBodyBinary above.
std::string Match::buildStateBinary(uint32_t tick, uint32_t lastSeq,
                                    const std::string& body) {
    std::string b;
    b.reserve(16 + body.size());
    nb::putU8(b, nb::STATE_BIN_VERSION);
    nb::putU32(b, tick);
    nb::putU32(b, lastSeq);
    b += body;
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
    explicit Session(tcp::socket socket) : ws_(std::move(socket)) {
        // Read the peer address once, here, while the socket is certainly open.
        // Asking later (at Accept, after two async hops) can fail on a connection
        // that has already gone, and this is the only identity E2's per-address
        // creation budget has. Errors leave it empty, which that budget treats as
        // "unknown address" rather than as everyone sharing one bucket.
        boost::system::error_code ec;
        const auto ep = ws_.next_layer().remote_endpoint(ec);
        if (!ec) remoteAddr_ = ep.address().to_string();
    }

    void Start() {
        // Read the HTTP upgrade request ourselves (instead of letting
        // async_accept consume it) so the join gate can check ?key= in the URL
        // BEFORE the WebSocket handshake completes. A wrong/missing key just
        // returns: the socket destructs and the connection closes without a
        // single byte answered - a scanner learns nothing.
        http::async_read(ws_.next_layer(), buffer_, req_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return; // not even HTTP; drop silently
                if (!websocket::is_upgrade(self->req_)) { self->MaybeServeStatus(); return; }
                if (!JoinKeyOk(QueryParam(std::string(self->req_.target()), "key"))) {
                    std::cout << "WS join rejected (bad key)\n";
                    return;
                }
                self->Accept();
            });
    }

    // A plain GET /status - not a WebSocket upgrade - answers with a small JSON
    // health blob and closes. Free monitoring, and it answers "is the running
    // binary the one I deployed?" without an SSH session, because it reports the
    // protocol tags the server is actually speaking.
    //
    // GATED BY THE JOIN KEY when one is set. PLATFORMZ_KEY exists so a scanner
    // sees a dead port; an endpoint that cheerfully described the server would
    // undo that, so without the key this stays as silent as every other path.
    //
    // Fixed shape, and nothing from the request is echoed back - a reply must
    // never be a way to get the server to repeat attacker-chosen bytes. TCP has
    // already proved the caller's address by this point, so unlike the UDP paths
    // there is no amplification concern.
    void MaybeServeStatus() {
        const std::string target(req_.target());
        if (req_.method() != http::verb::get) return;
        if (target.rfind("/status", 0) != 0) return;
        if (!JoinKeyOk(QueryParam(target, "key"))) return;

        const MatchRegistry::Totals t = g_registry.Summarise();
        std::string body = "{";
        body += "\"uptime\":"   + ji(g_uptimeSeconds.load());
        body += ",\"matches\":" + ji(t.matches);
        body += ",\"active\":"  + ji(t.active);
        body += ",\"players\":" + ji(t.players);
        body += ",\"maxMatches\":" + ji(MATCH_MAX_CONCURRENT);
        body += ",\"maxActive\":"  + ji(g_maxActiveMatches);
        body += ",\"maxPlayers\":" + ji(GAMESPACE_NUMBER_OF_PLAYERS);
        // The deployed-binary question: these must match the client's netbin.h.
        body += ",\"stateTag\":"   + ji((int)nb::STATE_BIN_VERSION);
        body += ",\"welcomeTag\":" + ji((int)nb::WELCOME_BIN_VERSION);
        body += ",\"egressBytes\":" + std::to_string(g_egress.bytes.load());
        body += "}";

        auto res = std::make_shared<http::response<http::string_body>>(
            http::status::ok, req_.version());
        res->set(http::field::content_type, "application/json");
        res->keep_alive(false);
        res->body() = std::move(body);
        res->prepare_payload();

        // Keep both the response and the session alive until the write finishes;
        // the socket closes when the last reference drops.
        http::async_write(ws_.next_layer(), *res,
            [self = shared_from_this(), res](beast::error_code, std::size_t) {});
    }

    // Key passed (or gate off): finish the WebSocket handshake from the
    // already-read upgrade request, then claim a slot and welcome the client.
    void Accept() {
        ws_.async_accept(req_, [self = shared_from_this()](beast::error_code ec) {
            if (ec) { std::cerr << "accept: " << ec.message() << "\n"; return; }

            // ?match=CODE on the upgrade URL picks the room - that is how an
            // invite link works. Absent is the ordinary case and means exactly
            // that: SeatOrPark parks the connection and the player picks from the
            // directory. Unknown is refused with `notfound`, parked either way.
            const std::string targetCode =
                clampName(QueryParam(std::string(self->req_.target()), "match"));
            // ?cid=UUID is the client's own install id (D1), carried on the
            // upgrade URL for the same reason ?key= is: a WebSocket claims its
            // slot here, during the handshake, long before any hello could
            // arrive. Absent for an older client, which simply never resumes
            // anything.
            const std::string cid =
                clampClientId(QueryParam(std::string(self->req_.target()), "cid"));
            // ?tok= is the identity token (D3), on the URL for the same reason
            // ?cid= is: a WebSocket client is welcomed the instant it connects,
            // so it may never send a hello at all - the handshake is the only
            // moment guaranteed to happen on this transport.
            const Identity ident =
                EstablishIdentity(QueryParam(std::string(self->req_.target()), "tok"));

            self->connId_ = nextConnId++;
            ConnectedClient c;
            c.clientId   = cid;
            c.identity   = ident.id;
            c.transport  = Transport::WS;
            c.session    = self;
            // Who this connection is, for the per-address creation budget. Taken
            // from the socket, never from anything the client said.
            c.remoteAddr = self->remoteAddr_;

            // Seating, the welcome and the leaderboard all live in SeatOrPark,
            // shared with the UDP path. A full room no longer ends the
            // connection: worst case this client is parked with no slot and told
            // so, and it stays here browsing until one frees up.
            // Before the welcome, so a client that is about to be parked with no
            // slot still ends up with an identity for next time.
            if (!ident.issue.empty()) self->Send(buildIdentity(ident.issue));

            SeatOrPark(self->connId_, c, targetCode);

            // Read() UNCONDITIONALLY, even unseated. This is E2's actual change
            // on this path: the old code returned here without reading, which is
            // what dropped the socket, which is what left the client re-helloing
            // into a server that was no longer listening to it.
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
    uint64_t connId_ = 0;   // this Session's key into `clients` (set in Accept)
    std::string remoteAddr_; // peer IP, captured at construction (see the ctor)

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
                    // Whichever room holds this connection, not necessarily the
                    // default one.
                    auto m = MatchForConn(self->connId_);
                    if (!m) m = g_defaultMatch;
                    {
                        std::lock_guard<std::mutex> lock(m->clientMutex);
                        auto it = m->clients.find(self->connId_);
                        if (it != m->clients.end()) {
                            std::cout << "Player " << it->second.playerId
                                      << " disconnected. Active: "
                                      << (m->clients.size() - 1) << "\n";
                            m->clients.erase(it);
                            m->connectedCount.store((int)m->clients.size());
                        }
                    }
                    // Clears the unseated record too: a client parked because the
                    // server was full still has one, and it must not outlive the
                    // socket it was reachable through.
                    ForgetConn(self->connId_);
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
    g_egress.Add(msg.size());   // A4: what actually leaves the box
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

//MARK: Routing helpers
static std::string buildJoinFail(const char* why);

// "You are connected, and you hold no slot."
//
// A client learns it is connected by receiving a WELCOME, which cannot exist
// without a seat - so a parked connection was indistinguishable from one whose
// handshake never landed, and the client's only recovery was to keep re-helloing
// forever. This is the seatless counterpart: it says the handshake DID land.
//
// Carries nothing. Why you are unseated is a separate question with a separate
// answer already (`joinfail`), and being unseated because you have not chosen a
// room yet (C6b) is not a failure at all.
//
// JSON on both transports, like everything except the welcome and the per-tick
// state - so no binary tag and no STATE_BIN_VERSION bump.
static std::string buildUnseated() { return "{\"type\":\"unseated\"}"; }

// The match a connection currently belongs to, or nullptr if it has none.
static std::shared_ptr<Match> MatchForConn(uint64_t connId) {
    std::string code;
    {
        std::lock_guard<std::mutex> lk(g_connMutex);
        auto it = g_connMatch.find(connId);
        if (it == g_connMatch.end()) return nullptr;
        code = it->second;
    }
    return code.empty() ? nullptr : g_registry.Find(code);
}

// Park a connection with no slot. It keeps its transport (session or UDP
// endpoint) so we can still talk to it, loses its playerId because it has none,
// and starts its idle clock now - an unseated UDP peer is reaped on the same
// silence rule a seated one is (see SweepUnseated).
//
// Then it is TOLD, which is the whole of C6a: `unseated` plus the leaderboard,
// the seatless mirror of the welcome-plus-leaderboard pair AttachConn sends.
static void ParkConn(uint64_t connId, ConnectedClient rec) {
    rec.playerId    = -1;
    rec.hasInput    = false;
    rec.lastInput   = PlayerInput{};
    rec.firePending = false;
    rec.lastSeenSec = NowSec();
    {
        std::lock_guard<std::mutex> lk(g_connMutex);
        g_unseated[connId] = rec;
        g_connMatch[connId].clear();
    }
    // Tell them, from HERE rather than from each call site - the same reason
    // AttachConn welcomes from one place: a new path cannot forget to. Off the
    // lock, like every other send site.
    SendToClient(rec, buildUnseated());
    // The board is not a property of a room, and HIGH SCORES is reachable from
    // the browser - so a seatless client gets it too. AttachConn sends the same
    // thing on the seated path.
    SendToClient(rec, buildLeaderboard(rec.identity));
}

// Drop every trace of a connection: its seat is the caller's business, this is
// the routing side. Called from both transports' disconnect paths, so a UDP
// endpoint can be claimed again and an unseated record cannot outlive its
// socket.
static void ForgetConn(uint64_t connId) {
    std::lock_guard<std::mutex> lk(g_connMutex);
    auto it = g_unseated.find(connId);
    if (it != g_unseated.end() && it->second.transport == Transport::UDP)
        g_udpIndex.erase(it->second.udpEndpoint);
    g_connMatch.erase(connId);
    g_unseated.erase(connId);
}

// Drop unseated UDP peers that have gone quiet, once a second from the driver
// loop. A parked client is not silent - it is re-sending hello every 0.5s
// looking for a seat - so silence here means the same thing it means in a lobby.
// WS connections are left alone: their socket closing is an event, and Read()'s
// error path already calls ForgetConn.
//
// Without this, being full would leak: every spoof-proof stranger that arrived
// while the server had no room would sit in g_unseated and g_udpIndex forever.
static void SweepUnseated() {
    const double now = NowSec();
    std::lock_guard<std::mutex> lk(g_connMutex);
    for (auto it = g_unseated.begin(); it != g_unseated.end(); ) {
        if (it->second.transport == Transport::UDP &&
            now - it->second.lastSeenSec > UDP_CLIENT_TIMEOUT_LOBBY) {
            g_udpIndex.erase(it->second.udpEndpoint);
            g_connMatch.erase(it->first);
            it = g_unseated.erase(it);
        } else {
            ++it;
        }
    }
}

// Take a connection out of whatever match holds it, returning its record so the
// caller can put it somewhere else. The slot it vacates is left to the match's
// own per-tick reconcile, exactly as a disconnect is.
//
// An UNSEATED connection detaches too, from g_unseated - that is what lets
// someone parked by a full server join a room the moment one frees up, using
// the same join path as everybody else.
static bool DetachConn(uint64_t connId, ConnectedClient& out) {
    auto m = MatchForConn(connId);
    if (!m) {
        std::lock_guard<std::mutex> lk(g_connMutex);
        auto it = g_unseated.find(connId);
        if (it == g_unseated.end()) return false;
        out = it->second;
        g_unseated.erase(it);
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(m->clientMutex);
        auto it = m->clients.find(connId);
        if (it == m->clients.end()) return false;
        out = it->second;
        m->clients.erase(it);
        m->connectedCount.store((int)m->clients.size());
    }
    std::lock_guard<std::mutex> lk(g_connMutex);
    g_connMatch[connId].clear();
    return true;
}

// Put a connection into a match and tell it which slot it got. A client learns
// "which body is mine" ONLY from a welcome, so binding without sending one leaves
// it rendering and steering as whatever slot it held before.
// `why` receives the wire token for a refusal (see joinFailureFromWire in
// wire.h, which is how the client turns it into something readable).
static bool AttachConn(uint64_t connId, const std::string& code,
                       ConnectedClient rec, const char*& why) {
    auto m = g_registry.Find(code);
    if (!m) { why = "notfound"; return false; }

    int slot = -1, seated = 0;
    {
        std::lock_guard<std::mutex> gg(m->gameMutex);
        std::lock_guard<std::mutex> gc(m->clientMutex);
        m->ReapIdleUdpClients();
        // rec carries the clientId forward from the room we are leaving, so
        // hopping back into a room we dropped out of resumes the held body.
        const Match::Seat seat = m->SeatPlayer(rec.clientId, rec.name);
        slot = seat.slot;
        if (slot != -1) {
            rec.playerId  = slot;
            rec.hasInput  = false;      // never carry aim or a fire latch across rooms
            rec.lastInput = PlayerInput{};
            rec.firePending = false;
            // Re-apply our name onto the new slot - but only if we have one. A
            // connection that has not named itself yet (a fresh WS client, which
            // sends its name only after the welcome tells it its slot) would
            // otherwise blank the slot's "PLAYER N" default.
            rec.nameDirty = !rec.name.empty();
            rec.lastSeenSec = NowSec(); // arrive alive, not with the old room's stamp
            m->clients[connId] = rec;
            m->connectedCount.store((int)m->clients.size());
            seated = (int)m->clients.size();
        }
    }
    if (slot == -1) { why = "full"; return false; }
    // The one place a connection takes a seat, so the one place worth logging it
    // - connect and room-hop both land here.
    std::cout << "conn " << connId << " ("
              << (rec.transport == Transport::UDP ? "udp" : "ws")
              << ") -> slot " << slot << " in " << code
              << ". Active there: " << seated
              // The IDENTITY, never the token: this half is safe to write down,
              // which is exactly why the two are separate strings. Eight chars is
              // enough to correlate a session by eye and useless for anything else.
              << (rec.identity.empty() ? "" : "  id " + rec.identity.substr(0, 8))
              << "\n";

    // Seated, so it is no longer unseated - the two are exclusive by
    // construction, and doing it here rather than at each call site means a new
    // path cannot leave a connection in both places.
    { std::lock_guard<std::mutex> lk(g_connMutex);
      g_connMatch[connId] = code;
      g_unseated.erase(connId); }
    // Off every match lock, like every other send site.
    SendToClient(rec, m->welcomeFor(rec));
    SendToClient(rec, buildLeaderboard(rec.identity));
    return true;
}

// Move a connection into `code`, refusing with a reason the client can render.
//
// Private rooms need their join code. NOTE that a wrong one is NOT refused the
// same way a missing room is - this comment used to claim it was, and the two
// lines below plainly send different tokens. `badcode` therefore tells a prober
// that a private room with that code exists, which makes the reply an existence
// oracle over the 4-character space; E1's five-guesses-a-minute budget is the
// only thing bounding it. Slow rather than serious (~1M codes), and worth
// collapsing into one reason - but that is a deliberate change to a
// security-relevant answer, not a comment fix. See docs/matchmaking.md.
//
// `outWhy`, when given, receives the refusal token (nullptr means it worked).
// Only the client-driven `join` verb wants it, to charge a wrong guess against
// E1's bad-code budget - the moves the SERVER initiates (create, quick, leave)
// pass codes it just produced itself and must not be charged for them.
static void MoveConnToMatch(uint64_t connId, const ConnectedClient& caller,
                            const std::string& code, const std::string& joinCode,
                            const char** outWhy = nullptr) {
    ConnectedClient rec;
    MatchEntry entry;
    const char* why = "unknown";
    if (outWhy) *outWhy = nullptr;

    // Refuse to the sink the CALLER already handed us, rather than looking the
    // connection up again: when that lookup missed it sent to a
    // default-constructed record - no session, no endpoint - so the refusal went
    // nowhere and the client just waited. Silence is the one thing a refusal must
    // never be.
    auto refuse = [&](const char* token) {
        if (outWhy) *outWhy = token;
        SendToClient(caller, buildJoinFail(token));
    };

    if (!g_registry.FindEntry(code, entry)) { refuse("notfound"); return; }
    if (entry.isPrivate && entry.joinCode != joinCode) { refuse("badcode"); return; }

    // Already there: re-welcome rather than churn the roster, so a duplicate join
    // is harmless instead of costing the player their slot.
    if (auto cur = MatchForConn(connId)) {
        if (cur == entry.match) {
            std::lock_guard<std::mutex> lock(cur->clientMutex);
            auto it = cur->clients.find(connId);
            if (it != cur->clients.end()) SendToClient(it->second, cur->welcomeFor(it->second));
            return;
        }
    }

    if (!DetachConn(connId, rec)) { refuse("notfound"); return; }
    if (!AttachConn(connId, code, rec, why)) {   // logs the seat itself
        // Put them back where they were rather than stranding them nowhere -
        // and if even the default room has no seat, park them unseated. What we
        // must never do is leave a live connection in neither place, which is
        // what the old "try the default and hope" line did on a full server.
        const char* ignored = "";
        if (!AttachConn(connId, g_defaultCode, rec, ignored)) ParkConn(connId, rec);
        refuse(why);
        return;
    }
}

// Seat a connection in the room it ASKED for, or in no room at all. Never in
// some third room it did not choose.
//
//   - named a room, and it had a seat   -> welcome, nothing else said;
//   - named a room it cannot have       -> parked, plus the reason (`notfound`
//     or `full`), so the browser can say what happened;
//   - named nothing                     -> parked, and that is not a failure.
//
// That last case is C6b, and it is the whole point: arriving without naming a
// room used to drop you into a landing room chosen for you. You now land in the
// directory and pick. ParkConn does the telling (`unseated` + the leaderboard),
// so a client can always tell "connected, holding nothing" from a handshake that
// never landed.
//
// There is deliberately NO fallback room on refusal. A consolation seat in a
// room nobody asked for is exactly what this removes, and the browser is already
// where a refusal belongs.
//
// Being parked is still not a dropped connection - that was E2's change and it
// stands: still connected, still able to list, join, create and quick.
//
// Shared by both transports' connect paths, so "what happens when you cannot
// have the room you asked for" has exactly one answer.
static void SeatOrPark(uint64_t connId, const ConnectedClient& rec,
                       const std::string& wantCode) {
    if (wantCode.empty()) { ParkConn(connId, rec); return; }

    const char* why = "";
    if (AttachConn(connId, wantCode, rec, why)) return;
    ParkConn(connId, rec);
    SendToClient(rec, buildJoinFail(why));
}

//MARK: Abuse budgets (E1, E2)
// -------------------------------------------------------------------------
// The cookie above proves an address is real. These budgets bound what a real
// address can then make the server do - because "the packets come from a machine
// that exists" is not the same as "the packets are in good faith".
//
// Most are keyed by CONNECTION, not by match: a connection can hop rooms, and a
// budget you can reset by joining somewhere else is not a budget. They live here
// rather than in ConnectedClient because that record is copied out from under a
// lock at every call site (and copied wholesale between matches on a move), so a
// counter living in it would be incremented on a temporary and thrown away.
//
// Room CREATION is the exception: it is keyed by source address, because the
// abuse there is one machine opening connection after connection and minting a
// room on each until the registry is full. See g_addrBudgets.

// The token bucket every limit below is built on lives in bucket.h, so it can be
// unit-tested against a controlled clock (server/test/bucket_test.cpp) - which is
// how the "a freshly booted server hands out a partial budget" bug was caught,
// and the only way it could have been.
using pz::Bucket;
using pz::TakeToken;

struct ConnBudget {
    Bucket list;                 // match-list replies
    Bucket move;                 // join / quick / leave - anything that re-seats us
    double joinWindow   = 0.0;   // start of the minute the bad joins below are counted in
    int    badJoins     = 0;
    double touchedSec   = 0.0;   // for the sweep; see PruneBudgets
};
static std::map<uint64_t, ConnBudget> g_budgets;

struct AddrBudget {
    Bucket create;
    double touchedSec = 0.0;
};
static std::map<std::string, AddrBudget> g_addrBudgets;

static std::mutex g_budgetMutex;   // guards both maps

// One match list per second per connection, sustained (E1). The reply is the
// largest thing an authenticated client can ask for on demand (B1 caps it at
// ~1.2 KB), so answering every request is exactly the amplification the cookie
// just closed, re-opened one handshake later. Three tokens covers a person
// opening the browser and paging twice inside a second.
const double LIST_REFILL_PER_SEC = 1.0;
const double LIST_BURST          = 3.0;

// Room moves per second per connection (E2). Every successful move costs a
// welcome plus a leaderboard - the two biggest packets the server sends - and
// churns a roster that other people are looking at. Five covers a player
// thumbing down the browser trying rooms; it does not cover a script.
const double MOVE_REFILL_PER_SEC = 1.0;
const double MOVE_BURST          = 5.0;

// Rooms one ADDRESS may mint (E2). This is the "anyone can make unlimited
// matches" abuse: the registry holds MATCH_MAX_CONCURRENT rooms, so without this
// one machine fills it and nobody else can create anything until the reaper
// catches up. Three in hand and one back every two minutes is generous for a
// person (who makes a room, plays in it, and makes another when that one ends)
// and useless for a script, which needs 24 minutes to monopolise a registry that
// reaps empty rooms in a fraction of that.
//
// An address is a COARSE identity, and this is the one place that matters: a LAN
// party, an office, or a household all arrive from one NAT and share one bucket,
// so the fourth person to try to make a room would be refused for something
// somebody else did. That is a real scenario, not a hypothetical, which is why
// PLATFORMZ_MAX_ROOMS_PER_ADDR exists - raise it, or set it to 0 to turn the
// budget off entirely on a server whose players you know.
const double CREATE_REFILL_PER_SEC     = 1.0 / 120.0;
const int    CREATE_BURST_DEFAULT      = 3;
int          g_maxRoomsPerAddr         = CREATE_BURST_DEFAULT;   // 0 = no budget

// Bad join codes per minute (E1). The code space is 4 characters, which a script
// walks in seconds if every guess gets an answer.
const int    BAD_JOIN_PER_MINUTE   = 5;
const double BAD_JOIN_WINDOW_SEC   = 60.0;

// Connection ids only ever go up and addresses come and go, so these maps would
// otherwise grow for the life of the process - slowly, but a server meant to run
// for months has no "slowly" that is fine. Sweep entries nothing has touched in
// a while, and only when a map is big enough for the walk to be worth it.
static void PruneBudgets(double now) {   // caller holds g_budgetMutex
    if (g_budgets.size() >= 512) {
        for (auto it = g_budgets.begin(); it != g_budgets.end(); ) {
            if (now - it->second.touchedSec > 120.0) it = g_budgets.erase(it);
            else ++it;
        }
    }
    // Addresses are swept on a much longer horizon than connections, because the
    // create bucket refills over minutes: drop an address at two minutes and its
    // budget would come back full every time it reconnected, which is exactly
    // what it is there to prevent.
    if (g_addrBudgets.size() >= 512) {
        for (auto it = g_addrBudgets.begin(); it != g_addrBudgets.end(); ) {
            if (now - it->second.touchedSec > g_maxRoomsPerAddr / CREATE_REFILL_PER_SEC)
                it = g_addrBudgets.erase(it);
            else ++it;
        }
    }
}

// True if this connection's `list` should be answered. False means drop it on
// the floor SILENTLY - not "too fast, try later", because an error reply is
// still a reply and a rate limiter that answers is not a rate limiter.
static bool AllowListReply(uint64_t connId) {
    const double now = NowSec();
    std::lock_guard<std::mutex> lk(g_budgetMutex);
    PruneBudgets(now);
    ConnBudget& b = g_budgets[connId];
    b.touchedSec = now;
    return TakeToken(b.list, now, LIST_BURST, LIST_REFILL_PER_SEC);
}

// True if this connection may be re-seated right now (join / quick / leave).
// Unlike the list, a refusal here IS answered - the player pressed a button and
// is owed a reason - which is affordable because a `joinfail` is ~40 bytes
// against the welcome it is declining to send.
static bool AllowMove(uint64_t connId) {
    const double now = NowSec();
    std::lock_guard<std::mutex> lk(g_budgetMutex);
    PruneBudgets(now);
    ConnBudget& b = g_budgets[connId];
    b.touchedSec = now;
    return TakeToken(b.move, now, MOVE_BURST, MOVE_REFILL_PER_SEC);
}

// True if this address may mint another room. An empty address (we could not
// read the peer's, which should not happen) is allowed through rather than
// sharing one bucket with every other unknown - a budget that punishes a
// bookkeeping failure would be a strange thing to debug.
static bool AllowCreate(const std::string& addr) {
    if (g_maxRoomsPerAddr <= 0) return true;   // budget turned off by the operator
    if (addr.empty()) return true;
    const double now = NowSec();
    std::lock_guard<std::mutex> lk(g_budgetMutex);
    PruneBudgets(now);
    AddrBudget& b = g_addrBudgets[addr];
    b.touchedSec = now;
    return TakeToken(b.create, now, (double)g_maxRoomsPerAddr, CREATE_REFILL_PER_SEC);
}

// True if this connection may try a join code at all.
//
// Note what this gates: EVERY attempt once the budget is spent, not just the
// wrong ones. It has to. Whether a code is a guess is only knowable after the
// registry lookup that answers the guess - so a limiter that let "good" codes
// through would be a limiter that answers every guess, which is no limiter at
// all. The cost is real and deliberate: mistype a code five times and you wait
// out the minute before the right one is accepted. That is the same bargain
// every login lockout makes, and the window is 60 seconds, not an hour.
static bool AllowJoinAttempt(uint64_t connId) {
    const double now = NowSec();
    std::lock_guard<std::mutex> lk(g_budgetMutex);
    PruneBudgets(now);
    ConnBudget& b = g_budgets[connId];
    b.touchedSec = now;
    if (now - b.joinWindow >= BAD_JOIN_WINDOW_SEC) { b.joinWindow = now; b.badJoins = 0; }
    return b.badJoins < BAD_JOIN_PER_MINUTE;
}

// Charge one failed guess. Only wrong/unknown codes are charged: hopping
// between rooms, or bouncing repeatedly off one that is full or already playing,
// means you named a room that exists - normal behaviour while waiting for a
// seat, and nothing a brute-forcer gets to do.
static void NoteBadJoin(uint64_t connId) {
    const double now = NowSec();
    std::lock_guard<std::mutex> lk(g_budgetMutex);
    ConnBudget& b = g_budgets[connId];
    b.touchedSec = now;
    if (now - b.joinWindow >= BAD_JOIN_WINDOW_SEC) { b.joinWindow = now; b.badJoins = 0; }
    b.badJoins++;
}

// Nothing erases a connection entry on disconnect on purpose. Connection ids are
// never reused, so a returning player always arrives on a fresh one and can
// never inherit a spent budget - which leaves the sweep above as the only
// cleanup anyone has to remember, instead of one more thing every disconnect
// path (WS close, goodbye, idle reap, room move) would have to call and could
// forget. The address entries deliberately DO survive a reconnect; that is the
// entire point of keying them by address.

//MARK: Directory
// The verbs that operate on the ROOM LIST rather than on a match: what exists,
// and making a new one. Entering a room is A3's job - it needs the connection ->
// match routing that does not exist yet - so `join`, `quick` and `leave` are not
// handled here.
//
// Reply size is capped so it stays one datagram. That was once a correctness
// requirement (a chunked list could destroy an in-flight welcome against the old
// single-slot reassembler) but #100 fixed that; it is now about keeping an
// unauthenticated UDP `list` from being an amplification vector, which E1 closes
// properly.
const size_t DIR_LIST_BUDGET_BYTES = 1160;   // under UDP_SAFE_DATAGRAM with slack
// Rows per page. Deliberately BELOW MATCH_MAX_CONCURRENT: set at or above it and
// every list fits one page, so the paging path never runs and quietly rots until
// the day the match cap is raised - at which point the browser would silently
// show a truncated list. 8 keeps paging on the tested path from day one, and
// smaller replies also shrink the amplification window E1 has to close.
const int    DIR_LIST_MAX_ROWS     = 8;
static_assert(DIR_LIST_MAX_ROWS < MATCH_MAX_CONCURRENT,
              "page size must stay under the match cap or paging is unreachable");

static std::string buildMatchList(int cursor) {
    std::vector<MatchListing> all = g_registry.List(/*includePrivate*/ false);
    // Stable order, so paging can't show the same room twice or skip one as
    // rooms come and go between requests.
    std::sort(all.begin(), all.end(),
              [](const MatchListing& a, const MatchListing& b) { return a.code < b.code; });

    if (cursor < 0) cursor = 0;
    std::string rows;
    int i = cursor, emitted = 0;
    for (; i < (int)all.size() && emitted < DIR_LIST_MAX_ROWS; ++i) {
        const MatchListing& r = all[i];
        std::string row = "{\"c\":"   + js(r.code)
                        + ",\"n\":"   + js(r.name)
                        + ",\"pre\":" + js(r.presetName)
                        + ",\"k\":"   + js(matchKindWire(r.kind))
                        + ",\"map\":" + js(r.mapSize)
                        + ",\"ph\":"  + js(phaseString(r.phase))
                        + ",\"p\":"   + ji(r.players)
                        + ",\"max\":" + ji(r.maxPlayers)
                        + ",\"j\":"   + jb(r.joinable) + "}";
        // Stop before overrunning the datagram rather than after.
        if (rows.size() + row.size() + 2 > DIR_LIST_BUDGET_BYTES) break;
        if (!rows.empty()) rows += ",";
        rows += row;
        emitted++;
    }
    const bool more = i < (int)all.size();
    return std::string("{\"type\":\"matchlist\",\"cur\":") + ji(cursor)
         + ",\"next\":" + ji(more ? i : -1)
         + ",\"total\":" + ji((int)all.size())
         + ",\"m\":[" + rows + "]}";
}

// Give a freshly created room its lobby: player slots and a welcome fragment,
// but no world - the world is generated when the match starts. Every room needs
// this before anyone can be told about it, or a joiner gets a welcome with no
// slots in it. Boot does the same thing for its resident rooms.
static void PrimeLobby(Match& m) {
    std::lock_guard<std::mutex> lock(m.gameMutex);
    m.gameSpace.spawnPlayers();
    m.rosterSize.store((int)m.gameSpace.getPlayers().size());
    m.rebuildWelcomeStatic();
}

// Tell the creator which room they just made. Load-bearing for a PRIVATE room:
// it is hidden from the match list, so this is the only place its code is ever
// revealed. Without it you could create a room and have no way to invite anyone.
static std::string buildCreated(const std::string& code) {
    return std::string("{\"type\":\"created\",\"m\":") + js(code) + "}";
}

static std::string buildJoinFail(const char* why) {
    return std::string("{\"type\":\"joinfail\",\"why\":") + js(why) + "}";
}

// Returns true if the message was a directory verb (handled here, or explicitly
// refused), false if it belongs to a match.
static bool HandleDirectoryMessage(uint64_t connId, const ConnectedClient& c,
                                  const std::string& msg) {
    if (msg.find("\"type\":\"list\"") != std::string::npos) {
        // Budgeted (E1): a small burst, then one list a second, and a request
        // over budget is dropped without a word. The browser asks once when it
        // opens and once per page, so a real client never notices; a script
        // asking 10,000 times a second gets one reply a second.
        if (!AllowListReply(connId)) return true;
        SendToClient(c, buildMatchList((int)parseUInt(msg, "cur", 0)));
        return true;
    }

    if (msg.find("\"type\":\"create\"") != std::string::npos) {
        // Budgeted per ADDRESS, not per connection (E2): one machine opening a
        // fresh connection for each room is exactly the abuse, so a per-connection
        // budget would be free to sidestep. Refused as server_full, which is what
        // it amounts to from where the player is standing - there is no room for
        // them to make - and is already a sentence the client knows how to say.
        if (!AllowCreate(c.remoteAddr)) {
            SendToClient(c, buildJoinFail("server_full"));
            std::cout << "Create refused: address " << c.remoteAddr
                      << " is over its budget\n";
            return true;
        }
        // clampName, the same one player names go through: printable ASCII only,
        // capped at PLAYER_NAME_MAX_CHARS. A room name is rendered in everybody's
        // browser, so control characters and unbounded length are not somebody
        // else's problem.
        const std::string name    = clampName(parseString(msg, "n"));
        const std::string preset  = clampName(parseString(msg, "pre"));
        const std::string code    = clampName(parseString(msg, "code"));
        const bool        isPriv  = parseBool(msg, "priv", false);

        // A player-created room is ALWAYS custom: they made it, they host it,
        // they set the rules and press START. `priv` chooses only whether it is
        // advertised in the browser - a public custom room is a normal thing to
        // want, and deriving governance from that flag is what #107 removed.
        //
        // There is deliberately no way to ask for an OFFICIAL room here. Only the
        // server mints those, or the preset they promise would guarantee nothing.
        MatchRegistry::CreateResult why;
        std::string newCode;
        auto m = g_registry.Create(name, preset.empty() ? "DEFAULT" : preset,
                                   MatchKind::Custom, isPriv, code,
                                   newCode, why);
        if (!m) {
            SendToClient(c, buildJoinFail("server_full"));
            std::cout << "Create refused: at capacity (" << g_registry.Size() << ")\n";
            return true;
        }
        PrimeLobby(*m);
        std::cout << "Match " << newCode << " created (custom, "
                  << (isPriv ? "invite-only" : "public")
                  << ") preset=" << (preset.empty() ? "DEFAULT" : preset) << "\n";
        // Tell them the code, then put them in it - making a room and not being in
        // it would be a strange thing to offer.
        SendToClient(c, buildCreated(newCode));
        // Join with the room's EFFECTIVE password, not the (empty) one they sent.
        // A private room with no password of its own is gated by its own code, so
        // passing theirs back had the server refuse the creator entry to the room
        // it had just built for them - CREATE looked like it did nothing at all.
        MoveConnToMatch(connId, c, newCode,
                        (isPriv && code.empty()) ? newCode : code);
        // Host AFTER the move, not before: MoveConnToMatch can still refuse (a
        // full or vanished room), and stamping first would leave a room hosted by
        // someone who never got into it. Set explicitly rather than left to
        // ResolveHostLocked's lowest-slot fallback - in a fresh room the creator
        // IS the lowest slot, so the two agree today, but the fallback is a
        // recovery path and this is a statement of intent.
        if (MatchForConn(connId) == m) {
            std::lock_guard<std::mutex> lock(m->clientMutex);
            m->hostConn = connId;
        }
        return true;
    }

    if (msg.find("\"type\":\"join\"") != std::string::npos) {
        const std::string want = clampName(parseString(msg, "m"));
        const std::string code = clampName(parseString(msg, "code"));
        // Two separate gates, both refusing with rate_limited. AllowMove is E2's
        // joins-per-second: every accepted move costs a welcome and a
        // leaderboard and churns a roster other people are watching.
        // AllowJoinAttempt is E1's brute-force budget, below.
        if (!AllowMove(connId)) {
            SendToClient(c, buildJoinFail("rate_limited"));
            return true;
        }
        // A room code is 4 characters. Answering every guess turns that into a
        // few seconds of scripting, so a connection gets BAD_JOIN_PER_MINUTE
        // wrong ones and is then told to wait - checked before the registry is
        // touched, so a refused attempt reveals nothing about what exists.
        if (!AllowJoinAttempt(connId)) {
            SendToClient(c, buildJoinFail("rate_limited"));
            return true;
        }
        const char* why = nullptr;
        MoveConnToMatch(connId, c, want, code, &why);
        // Only a guess counts. "Full" or "in progress" means they named a real
        // room and simply could not get in, which is a normal thing to do
        // repeatedly while waiting for a seat.
        if (why && (std::strcmp(why, "notfound") == 0 || std::strcmp(why, "badcode") == 0))
            NoteBadJoin(connId);
        return true;
    }

    if (msg.find("\"type\":\"quick\"") != std::string::npos) {
        // Same move budget as `join` (E2) - QUICK MATCH re-seats you exactly the
        // same way, and would otherwise be the cheap way around it.
        if (!AllowMove(connId)) { SendToClient(c, buildJoinFail("rate_limited")); return true; }
        // Fullest joinable OFFICIAL lobby, else make one. One round trip, and the
        // "fullest" rule packs players together instead of scattering one each
        // across empty rooms.
        //
        // Official only, on purpose. QUICK MATCH promises a game that starts:
        // dropping someone into a stranger's public custom room hands their
        // experience to a host who chose the rules and may never press START.
        // Someone who wants that room can still pick it out of the browser.
        //
        // Ties break on the PRESET ORDER in options.h, which runs from typical
        // gameplay to niche. On a quiet server every official room is equally
        // empty, so the tie IS the common case - and without this the winner was
        // whichever room List() happened to sort first, i.e. its randomly minted
        // 4-character code. That made a stranger's first game a coin flip
        // between the standard match and the one with no walls.
        std::string best;
        int    bestPlayers = -1;
        size_t bestRank    = matchOptionPresets.size();   // worse than any real preset
        for (const MatchListing& r : g_registry.List(/*includePrivate*/ false)) {
            if (r.kind != MatchKind::Official) continue;
            if (r.phase != Phase::LOBBY || !r.joinable) continue;
            const size_t rank = MatchPresetRank(r.presetName);
            // Fuller wins outright; equally full falls back to the ramp. Strictly
            // less-than on the rank so the first of two identical presets keeps
            // it, which makes the choice stable rather than last-one-wins.
            if (r.players > bestPlayers ||
                (r.players == bestPlayers && rank < bestRank)) {
                bestPlayers = r.players;
                bestRank    = rank;
                best        = r.code;
            }
        }
        if (best.empty()) {
            // The resident official rooms are pinned, so reaching here means they
            // are all full or already playing. Server-created, so still official.
            MatchRegistry::CreateResult why;
            auto m = g_registry.Create(MatchPresetByName("DEFAULT").label, "DEFAULT",
                                       MatchKind::Official, /*isPrivate*/ false, "",
                                       best, why);
            if (!m) { SendToClient(c, buildJoinFail("server_full")); return true; }
            PrimeLobby(*m);
            std::cout << "Match " << best << " created (official) for quick match\n";
        }
        MoveConnToMatch(connId, c, best, "");
        return true;
    }

    if (msg.find("\"type\":\"leave\"") != std::string::npos) {
        // Leaving re-seats you too, so it spends from the same bucket (E2).
        // Join-leave-join-leave is the cheapest roster churn there is, and
        // exempting the leave half would make the join half's budget meaningless.
        if (!AllowMove(connId)) { SendToClient(c, buildJoinFail("rate_limited")); return true; }
        // Out, and nowhere. Leaving used to hand you the landing room, because a
        // client with no room receives no state and looked frozen - the browser
        // is where a roomless client belongs, and ParkConn tells it so.
        //
        // Already unseated (a `leave` from the browser) is a no-op, not an error:
        // DetachConn says so by returning false and there is nothing to undo.
        ConnectedClient rec;
        if (DetachConn(connId, rec)) ParkConn(connId, rec);
        return true;
    }
    return false;
}

//MARK: Handle client message
// -------------------------------------------------------------------------
// Dispatch one inbound text frame from an already-registered client (WS or UDP).
// Shared by Session::Read and the UDP receive handler; runs on an io thread.
// New-UDP-peer registration (claiming a slot) happens in the UDP handler BEFORE
// this is called, so here the client always exists in `clients`.
// -------------------------------------------------------------------------
void Match::HandleMessage(uint64_t connId, const std::string& msg) {
    // Directory verbs first: they are about the room LIST, not this room, and a
    // connection can ask about them whatever match it happens to be in.
    //
    // Copy the sink out under the lock, then release it BEFORE writing to a
    // socket - the same discipline every other send site here follows, so a slow
    // client can never stall the sim behind clientMutex.
    {
        ConnectedClient sink;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(clientMutex);
            auto it = clients.find(connId);
            if (it != clients.end()) { sink = it->second; found = true; }
        }
        if (found && HandleDirectoryMessage(connId, sink, msg)) return;
    }

    //MARK: Msg: hello
    // Handshake / keepalive: (re)send the welcome to this client. UDP clients
    // resend hello until welcomed (unreliable transport); a WS client's hello
    // just re-welcomes harmlessly. A name may ride the hello. buildWelcome reads
    // gameSpace unlocked exactly as the WS connect-welcome does.
    if (msg.find("\"type\":\"hello\"") != std::string::npos) {
        ConnectedClient sink;
        std::string issue;
        {
            std::lock_guard<std::mutex> lock(clientMutex);
            auto it = clients.find(connId);
            if (it == clients.end()) return;
            std::string nm = clampName(parseString(msg, "name"));
            if (!nm.empty()) { it->second.name = nm; it->second.nameDirty = true; }
            // Identity (D3) is settled on the CONNECT paths, but it is settled
            // over UDP by a single datagram - and a datagram can be lost. A
            // client that never received the token it was issued would otherwise
            // carry on without one until its next session, so re-establish it
            // here too: the client is already re-sending hello for exactly this
            // reason, and a token that verifies costs one HMAC and changes
            // nothing.
            if (it->second.identity.empty() || !parseString(msg, "tok").empty()) {
                const Identity id = EstablishIdentity(parseString(msg, "tok"));
                if (!id.id.empty()) it->second.identity = id.id;
                issue = id.issue;
            }
            sink = it->second;
        }
        // Off the lock, like every other send site here.
        if (!issue.empty()) SendToClient(sink, buildIdentity(issue));
        SendToClient(sink, welcomeFor(sink));
        return;
    }

    //MARK: Msg: goodbye
    // Client is exiting intentionally (window close, or a caught SIGINT/SIGTERM -
    // see main.cpp). Free its slot right away instead of waiting out the UDP idle
    // timeout - the common "player just quit" case no longer has to sit in a
    // stale-but-still-connected state for up to UDP_CLIENT_TIMEOUT seconds. (WS
    // clients are already reaped promptly via Session::Read()'s TCP-error path;
    // handling this here too is harmless and keeps both transports symmetric.)
    if (msg.find("\"type\":\"goodbye\"") != std::string::npos) {
        std::lock_guard<std::mutex> lock(clientMutex);
        auto it = clients.find(connId);
        if (it != clients.end()) {
            std::cout << "Player " << it->second.playerId << " said goodbye. Active: "
                      << (clients.size() - 1) << "\n";
            {
                std::lock_guard<std::mutex> cl(g_connMutex);
                if (it->second.transport == Transport::UDP)
                    g_udpIndex.erase(it->second.udpEndpoint);
                // Also drop the routing entry, which used to be left behind: ids
                // are never reused, so every deliberate quit leaked one map node
                // for the life of the process.
                g_connMatch.erase(connId);
            }
            clients.erase(it);
            connectedCount.store((int)clients.size());
        }
        return;
    }

    //MARK: Msg: start
    // Control message: a client asking to start/restart a match. Only the host
    // (lowest connected slot) may start; ignore it from anyone else. Flagged here
    // and performed by the sim loop so all gameSpace mutation stays on a single
    // thread.
    if (msg.find("\"type\":\"start\"") != std::string::npos) {
        // A locked room (public) has no host controls at all - it starts itself
        // via ServiceAutoStart. Reject from everyone, not just non-hosts.
        if (optionsLocked) return;
        if (!isHostConn(connId)) return; // host-only; non-host clients have no START button, this is the backstop
        // Map is part of the options bundle now, so a start no longer carries
        // three loose numbers the lobby had never seen.
        pendingMap = MapSizeIndex(clampName(parseString(msg, "map")));
        pendingPlayers = (int)parseUInt(msg, "nplayers", GAMESPACE_DEFAULT_PLAYERS);
        pendingDiff = parseFloat(msg, "diff", BOT_DIFFICULTY_DEFAULT);
        pendingMaxBots = (int)parseUInt(msg, "maxbots", (unsigned)MAX_BOTS_DEFAULT);
        pendingMinHumans = (int)parseUInt(msg, "minhumans", (unsigned)PUBLIC_MIN_PLAYERS);
        pendingWallElast = parseFloat(msg, "welast", WALL_ELASTICITY_PLAYER);
        pendingPlatElast = parseFloat(msg, "pelast", PLATFORM_ELASTICITY_PLAYER);
        pendingBoost = parseFloat(msg, "boost", 1.0f);
        pendingRocketSpeed = parseFloat(msg, "rspeed", 1.0f);
        pendingXRadius = parseFloat(msg, "xradius", 1.0f);
        pendingJThrust = parseFloat(msg, "jthrust", 1.0f);
        pendingFuelBurn = (int)parseUInt(msg, "fburn", (unsigned)FUEL_CONSUMPTION_RATE);
        pendingFuelRegen = (int)parseUInt(msg, "fregen", (unsigned)FUEL_REGEN_PCT_DEFAULT);
        pendingWallsEnabled = parseBool(msg, "walls", WALLS_ENABLED);
        pendingRocketsPhysics = parseBool(msg, "phys", ROCKETS_OBEY_PHYSICS);
        pendingFriendlyFire = parseBool(msg, "ff", FRIENDLY_FIRE);
        pendingCoastMode = parseBool(msg, "coast", COAST_MODE);
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
        if (optionsLocked) return;       // public room: rules are fixed at creation
        if (!isHostConn(connId)) return; // host-only; matches the client's OPTIONS gating
        pendingPlayers = (int)parseUInt(msg, "nplayers", pendingPlayers.load());
        pendingDiff = parseFloat(msg, "diff", pendingDiff.load());
        pendingMaxBots = (int)parseUInt(msg, "maxbots", (unsigned)pendingMaxBots.load());
        pendingMinHumans = (int)parseUInt(msg, "minhumans", (unsigned)pendingMinHumans.load());
        pendingWallElast = parseFloat(msg, "welast", pendingWallElast.load());
        pendingPlatElast = parseFloat(msg, "pelast", pendingPlatElast.load());
        pendingBoost = parseFloat(msg, "boost", pendingBoost.load());
        pendingRocketSpeed = parseFloat(msg, "rspeed", pendingRocketSpeed.load());
        pendingXRadius = parseFloat(msg, "xradius", pendingXRadius.load());
        pendingJThrust = parseFloat(msg, "jthrust", pendingJThrust.load());
        pendingFuelBurn = (int)parseUInt(msg, "fburn", (unsigned)pendingFuelBurn.load());
        pendingFuelRegen = (int)parseUInt(msg, "fregen", (unsigned)pendingFuelRegen.load());
        pendingWallsEnabled = parseBool(msg, "walls", pendingWallsEnabled.load());
        pendingRocketsPhysics = parseBool(msg, "phys", pendingRocketsPhysics.load());
        pendingFriendlyFire = parseBool(msg, "ff", pendingFriendlyFire.load());
        pendingCoastMode = parseBool(msg, "coast", pendingCoastMode.load());
        // Live-editable like every other rule, so the lobby's map row updates on
        // everyone's screen the moment the host picks a different arena.
        {
            const std::string m = clampName(parseString(msg, "map"));
            if (!m.empty()) pendingMap = MapSizeIndex(m);
        }
        return;
    }

    //MARK: Msg: endmatch
    // Control message: the host pressed the end-match key (M). Host-only
    // (isHostConn - the lowest connected slot, "player 1"), like start/options.
    // PLAYING-only so a stray press can't disturb the lobby or countdown. The
    // phase flip reaches every client in the next tick's state broadcast, and
    // each runs its normal game-over sequence.
    if (msg.find("\"type\":\"endmatch\"") != std::string::npos) {
        if (optionsLocked) return;       // public room: no stranger may end it
        if (!isHostConn(connId)) return; // host-only; matches the client's gating
        if (gamePhase.load() == Phase::PLAYING) {
            gamePhase = Phase::GAMEOVER;
            std::cout << "Match ended by host request\n";
        }
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
            // Match-epoch gate. The client stamps every input with the last epoch
            // it saw in a state packet, so a packet built for the PREVIOUS match -
            // still in flight, or sent by a client that hasn't seen the restart
            // yet - is dropped here instead of overwriting the new match's spawn
            // state. It carries an ABSOLUTE yaw/pitch, so applying a stale one is
            // exactly the spawn-aim bug. ep == 0 means the client didn't stamp
            // one (a build predating this field); accept those unchanged.
            uint32_t ep = parseUInt(msg, "ep", 0);
            if (ep != 0 && ep != matchEpoch.load()) return;
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

// Router: find the match this connection belongs to, and forward.
//
// A connection is bound to at most one match at a time, so this is a lookup, not
// a broadcast. A packet for a room that has since been reaped falls back to the
// default room rather than being dropped - the client is real and still
// connected, it just has nowhere to be.
// Everything an unseated connection can do (E2). It holds no player slot, so the
// match verbs - input, start, name, endmatch - have no body to act on and are
// dropped. Two things do reach it:
//
//   - the directory (list/join/create/quick), which is how it gets out of here;
//   - `hello`, which is the client's existing handshake retry and doubles as
//     "is there a seat yet?". That is the whole recovery path: the client is
//     already re-sending hello every 0.5s while it has no slot, so a seat
//     freeing up is picked up within half a second with no new client code.
//
// Returns false only if this connection is not parked here at all, which lets
// the caller fall through to its old behaviour.
static bool HandleUnseatedMessage(uint64_t connId, const std::string& msg) {
    ConnectedClient sink;
    {
        std::lock_guard<std::mutex> lk(g_connMutex);
        auto it = g_unseated.find(connId);
        if (it == g_unseated.end()) return false;
        it->second.lastSeenSec = NowSec();   // liveness, for SweepUnseated
        sink = it->second;
    }

    if (HandleDirectoryMessage(connId, sink, msg)) return true;

    if (msg.find("\"type\":\"hello\"") != std::string::npos) {
        // A name may ride the hello exactly as it does on a first one. Keep it
        // even if we stay unseated, so the name is already right whenever a seat
        // does appear.
        const std::string nm = clampName(parseString(msg, "name"));
        if (!nm.empty()) { sink.name = nm; sink.nameDirty = true; }
        const std::string want = clampName(parseString(msg, "match"));

        // A BARE hello - no room named - is not a request for a seat. It is a
        // client sitting in the directory saying it is still there.
        //
        // Seating it here is what made parking circular: the server parks a
        // connection, and the client's own 0.5 s retry undoes that half a second
        // later, however carefully it was parked. Nothing could stay unseated on
        // purpose while this line existed.
        //
        // `sink` is a copy, so a name that rode this hello has to be written
        // back by hand. Re-ack, because over UDP the first one can simply have
        // been lost - but NOT the leaderboard, which it already has.
        if (want.empty()) {
            if (!nm.empty()) {
                std::lock_guard<std::mutex> lk(g_connMutex);
                auto it = g_unseated.find(connId);
                if (it != g_unseated.end()) { it->second.name = nm; it->second.nameDirty = true; }
            }
            SendToClient(sink, buildUnseated());
            return true;
        }
        SeatOrPark(connId, sink, want);
        return true;
    }

    if (msg.find("\"type\":\"goodbye\"") != std::string::npos) {
        ForgetConn(connId);
        return true;
    }

    // Anything else: swallowed. Returning true rather than falling through is
    // deliberate - without a slot there is no match this packet could belong to,
    // and routing it to the default room would have it looked up in a client map
    // it is not in and silently dropped there instead, which is the same outcome
    // by a longer road.
    return true;
}

static void HandleClientMessage(uint64_t connId, const std::string& msg) {
    auto m = MatchForConn(connId);
    // No room: parked with no seat (E2). Its own small dispatch, above.
    if (!m && HandleUnseatedMessage(connId, msg)) return;
    if (!m) m = g_defaultMatch;

    // Stamp liveness HERE, because only here do we know which match holds the
    // record. UDP has no disconnect event, so ReapIdleUdpClients culls anyone
    // unstamped for 3s in a lobby - and when endpoint->connId moved to a global
    // index, this stamp lost its home. The result was every UDP client being
    // reaped mid-session: joins looked like they worked and the player vanished
    // three seconds later.
    {
        std::lock_guard<std::mutex> lock(m->clientMutex);
        auto it = m->clients.find(connId);
        if (it != m->clients.end()) it->second.lastSeenSec = NowSec();
    }
    m->HandleMessage(connId, msg);
}

//MARK: Broadcast
// -------------------------------------------------------------------------
// Broadcast state to all connected clients.
// Every client sees the same world this tick - only `lastSeq` differs, so the
// (comparatively expensive) JSON/binary body is serialized ONCE per tick per
// transport here, not once per connected client; buildStatePacket/
// buildStateBinary then just prepend each client's own small tick/seq header
// onto that shared body.
//
// Called from the sim loop AFTER gameMutex is released, on purpose: the
// per-client build and blocking socket writes must not stall the simulation
// (or hold gameMutex while a slow/dead client backs up a write). Reading
// gameSpace here without gameMutex is safe because the sim thread is its only
// mutator and this runs on that same thread, sequentially after the locked
// sim step - so no concurrent writer exists. Only clientMutex is taken here,
// to guard the clients map against connects/disconnects on io threads.
// -------------------------------------------------------------------------
void Match::BroadcastState(uint32_t tick) {
    std::lock_guard<std::mutex> lock(clientMutex);
    // Which player slots are occupied, so clients can hide empty ones.
    SlotMask connectedSlots = 0;
    bool haveWs = false, haveUdp = false;
    for (auto& [cid, client] : clients) {
        SlotAdd(connectedSlots, client.playerId);
        if (client.transport == Transport::UDP) haveUdp = true;
        else haveWs = true;
    }
    // Resolved here, under the lock this already holds, so a host who left is
    // replaced on the very next packet rather than whenever someone next presses
    // something. -1 in an official room: nobody hosts it.
    const int hostSlot = HostSlotLocked();

    // Build each transport's shared body at most once, and only if a client
    // of that transport is actually connected.
    std::string jsonBody, binBody;
    if (haveWs)  jsonBody = buildStateBodyJson(connectedSlots, hostSlot);
    if (haveUdp) binBody  = buildStateBodyBinary(connectedSlots, hostSlot);

    for (auto& [cid, client] : clients) {
        // UDP gets the compact binary state (fits one datagram; MTU-safe over the
        // internet). WebSocket/TCP has no MTU limit, so it keeps the JSON state.
        std::string packet = (client.transport == Transport::UDP)
            ? buildStateBinary(tick, client.lastSeq, binBody)
            : buildStatePacket(tick, client.lastSeq, jsonBody);
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
                               float dt, float gravity, GameSpace& gameSpace) {
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

//MARK: A4 perf report
// One greppable line per interval. Deliberately a separate line rather than an
// extension of the heartbeat: A6 restructures the heartbeat for N matches, and
// the Actions idle-watchdog greps that line, so keeping them apart avoids
// breaking a job with an instrumentation change.
//
// The numbers to read: budget 10 ms per 60 Hz beat, NOT 16.6 - the rest goes to
// io threads, Caddy's TLS on the same box, and shared-CPU steal time. Concurrent
// matches a sequential scheduler can hold is roughly 10 / p95_total.
const int PERF_REPORT_SECONDS = 5;

static std::chrono::steady_clock::time_point g_perfLast{};
static uint64_t g_perfLastBytes = 0;

static void ReportPerf(Match& m, std::chrono::steady_clock::time_point now) {
    auto sim = m.statSim.Summarise();
    auto bc  = m.statBroadcast.Summarise();
    auto gr  = m.statGrid.Summarise();
    if (m.statSim.Empty()) return;

    const uint64_t bytes = g_egress.bytes.load(std::memory_order_relaxed);
    double secs = g_perfLast.time_since_epoch().count()
                ? std::chrono::duration<double>(now - g_perfLast).count() : 0.0;
    double kbs  = secs > 0.0 ? (bytes - g_perfLastBytes) / 1024.0 / secs : 0.0;
    g_perfLast = now; g_perfLastBytes = bytes;

    const double p95total = sim.p95 + bc.p95;
    int players; { std::lock_guard<std::mutex> gc(m.clientMutex); players = (int)m.clients.size(); }

    printf("PERF sim p50/p95/max %.2f/%.2f/%.2f ms | broadcast %.2f/%.2f/%.2f | "
           "grid %.2f/%.2f/%.2f | total p95 %.2f ms | egress %.0f KB/s | "
           "clients %d roster %d | fits ~%d matches\n",
           sim.p50, sim.p95, sim.max, bc.p50, bc.p95, bc.max, gr.p50, gr.p95, gr.max,
           p95total, kbs, players, (int)m.gameSpace.getPlayers().size(),
           p95total > 0.0 ? (int)(10.0 / p95total) : 0);
    fflush(stdout);
}

//MARK: Sim tick
// -------------------------------------------------------------------------
// One 60 Hz step for this match: consume a pending start, run the countdown,
// reap idle clients, tick the sim, then broadcast. Split out of SimulationLoop
// so the driver below can eventually beat several matches per tick (A4) - the
// body itself is unchanged.
// -------------------------------------------------------------------------
void Match::Tick(CollisionGrid& scratchGrid) {
    using Duration = std::chrono::duration<double>;
    const auto now = Clock::now();   // was the driver loop's local

    uint32_t tick;
    size_t   asteroidCount = 0;
    bool     justStarted = false; // a match began this tick -> resend welcomes below
    bool     leaderboardDirty = false; // scoreboard credited this tick -> broadcast below
    bool     wentLive    = false; // COUNTDOWN -> PLAYING flipped THIS tick -> drop every
                                  // client's stale input latch before the first apply
    double gridMs = 0.0;
    {
        ScopedTime _sim(statSim);
        std::lock_guard<std::mutex> gg(gameMutex);

        //MARK: Start match
        // Consume a pending start/restart request: build a fresh world and
        // reset the existing player slots (ids stay stable so connected
        // clients keep their slot mapping across a restart), then begin.
        // Capacity hold (E2). A start is DEFERRED, never dropped: leave
        // startRequested set and the room begins the moment a live match ends.
        // Off by default (see MATCH_MAX_ACTIVE_DEFAULT), so at the shipped
        // setting this test is a single integer compare that always passes.
        if (startRequested.load() && !ActiveMatchBudgetAllows(this)) {
            if (!startHeld) {
                startHeld = true;
                std::cout << "Match " << matchCode << " start held: "
                          << g_maxActiveMatches << " live matches already "
                          << "(PLATFORMZ_MAX_ACTIVE)\n";
            }
        } else if (startRequested.exchange(false)) {
            startHeld = false;
            // Compact the connected clients into the lowest slots, in their
            // current slot order (order-preserving, so the host - lowest
            // slot - stays the host). This closes the hole a mid-session
            // leaver left behind: the next human takes over their slot (and
            // its color), and the roster can shrink down to the humans that
            // are actually here instead of being floored by the highest
            // claimed slot. nameDirty re-applies each client's name to its
            // NEW slot via the per-tick name sync. Every client is told its
            // new slot by the justStarted welcome resend below.
            // (lock order gameMutex->clientMutex preserved.)
            int want = pendingPlayers.load(), connectedAtStart = 0;
            {
                std::lock_guard<std::mutex> gc(clientMutex);
                // Reap eagerly, not just on this tick's later periodic sweep:
                // a UDP client that already exceeded the idle timeout (e.g.
                // quit moments ago) must not get compacted into the fresh
                // roster as a "connected" human just because that sweep
                // hasn't run yet this tick.
                ReapIdleUdpClients();
                connectedAtStart = (int)clients.size();
                // Every client is welcomed unconditionally right below
                // (justStarted), so this call's own re-welcome list can be
                // ignored here - it only matters for the continuous LOBBY
                // compaction call site (see the per-tick "Bot reconcile"
                // block below).
                CompactConnectedSlots();
            }
            // Resize the roster to the requested match size, but never below
            // the number of connected humans (no one loses their body). Slots
            // beyond the humans become bots via the per-tick reconcile. Do this
            // BEFORE generate so resetPlayersForMatch/placePlayersSpread size to
            // the final count.
            want = std::min(std::max(want, connectedAtStart), GAMESPACE_NUMBER_OF_PLAYERS);
            // Clamp the preset's asteroid count to the UDP state-packet
            // budget for this roster, so a full tick fits one unfragmented
            // datagram (oversized ticks chunk lossily - see netbin.h).
            const mapSizePreset& mp = mapSizePresets.at(MapSizeName(pendingMap.load()));
            int roids = std::min(mp.numAsteroids, nb::MaxAsteroidsForRoster(want));
            if (roids < mp.numAsteroids)
                std::cout << "Asteroids clamped " << mp.numAsteroids << " -> " << roids
                          << " (UDP packet budget, " << want << " player slots)\n";
            gameSpace.configureMap(mp.halfSize, mp.numPlatforms, roids);
            gameSpace.setPlayerCount(want);
            rosterSize.store(want);   // the directory's joinable test reads this
            // OPTIONS: apply the requesting client's full options bundle to the
            // sim before the world is built - generatePlatforms() below stamps
            // PLATFORM ELASTICITY per-platform from the value applyOptions sets,
            // and collisions/movement read the rest off GameSpace.
            {
                MatchOptions o;
                o.wallElasticity      = pendingWallElast.load();
                o.platformElasticity  = pendingPlatElast.load();
                o.speedBoost          = pendingBoost.load();
                o.rocketSpeedScale    = pendingRocketSpeed.load();
                o.explosionRadiusScale = pendingXRadius.load();
                o.jetpackThrust       = pendingJThrust.load();
                o.fuelConsumption     = pendingFuelBurn.load();
                o.fuelRegenPct        = pendingFuelRegen.load();
                o.wallsEnabled        = pendingWallsEnabled.load();
                o.rocketsObeyPhysics  = pendingRocketsPhysics.load();
                o.friendlyFire        = pendingFriendlyFire.load();
                o.coastMode           = pendingCoastMode.load();
                o.mapSize             = MapSizeName(pendingMap.load());
                gameSpace.applyOptions(o);
            }
            // Issue #5 order: platforms -> players (spread) -> asteroids
            // (buffered away from the placed players). Same sequence the local
            // client's generate() uses, so both modes build worlds identically.
            gameSpace.generatePlatforms();
            gameSpace.resetPlayersForMatch();
            // Slot ownership is per-MATCH. resetPlayersForMatch deliberately
            // leaves leaveGraceSec alone (it belongs to the vacancy logic), so a
            // countdown armed by someone who left the previous match can still be
            // running on a slot in this one - and without clearing these, that
            // player rejoining would "resume" a body from a match they never
            // played and everyone would see a spurious RECONNECTED line.
            {
                std::lock_guard<std::mutex> cl(clientMutex);
                slotOwner.fill(std::string());
                for (auto& [cid, c] : clients)
                    if (c.playerId >= 0 && c.playerId < GAMESPACE_NUMBER_OF_PLAYERS)
                        slotOwner[c.playerId] = c.clientId;   // whoever is here NOW owns their slot
            }
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
            // New match: bump the epoch BEFORE leaving LOBBY/GAMEOVER so the
            // first packet clients see for this match already carries it.
            // Every input still stamped with the old epoch is now rejected.
            // Skips 0, which is reserved for "client didn't stamp one".
            if (++matchEpoch == 0) matchEpoch = 1;
            gameOverStamped = false; // fresh match: no wind-down pending
            gameOverSimIdle = false;
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
                wentLive  = true; // consumed by the input-apply block below
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
        // In the lobby the bot reconcile below turns the vacated slot into
        // a bot; mid-match the body stays open for a reconnect (see
        // refreshBotSlots). (WS clients are cleaned up by their
        // Read()-error callback instead.) See ReapIdleUdpClients() for why
        // this also runs eagerly at connect time, not just here.
        {
            std::lock_guard<std::mutex> gc(clientMutex);
            ReapIdleUdpClients();
        }

        //MARK: Auto-start (public rooms)
        // A locked public room has no host to press START; this is what begins
        // it. No-op for host-controlled rooms, which is every room until A3 can
        // create public ones.
        ServiceAutoStart(now);

        //MARK: Lobby slot compaction
        // Continuously close gaps left by a leaver, live - not just once at
        // match start. LOBBY only: once gameSpace.generate() has run
        // (COUNTDOWN onward), slot index also means a spawned body tied to
        // that index, and renumbering playerId without moving the body
        // would desync "which body is mine" from the client's new slot. In
        // pure LOBBY, gameSpace.getPlayers() is preview data only, so this
        // is safe. Every re-slotted client needs a fresh welcome - it only
        // learns "which slot is mine" (myIndex) from one.
        if (gamePhase.load() == Phase::LOBBY) {
            std::lock_guard<std::mutex> gc(clientMutex);
            for (uint64_t connId : CompactConnectedSlots()) {
                auto it = clients.find(connId);
                if (it != clients.end()) SendToClient(it->second, welcomeFor(it->second));
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
        // fill the match (and updates live as humans join/leave). Botifying is
        // withheld only while a match is actually LIVE (PLAYING): a mid-match
        // disconnect leaves the body OPEN (not bot-driven) so the player can
        // reconnect and resume it, while a mid-match join still flips a slot
        // bot->human (the bot yields). GAMEOVER counts as "forming" here too -
        // the client's screen already looks and behaves like the lobby at that
        // point (returnToTitle keeps the socket open and comes right back to
        // the roster), so a leaver's slot should stop showing their stale name
        // instead of sitting untouched until the next match start relabels it.
        // Cheap - a handful of slots. Bots are only DRIVEN in PLAYING/GAMEOVER
        // (below); in the lobby they just hold a slot. HandleMidMatchLeavers is
        // the PLAYING-only counterpart: once !allowBotify, it grace-counts and
        // eventually eliminates a body refreshBotSlots left open (see its own
        // comment).
        {
            Phase ph = gamePhase.load();
            bool allowBotify = (ph == Phase::LOBBY || ph == Phase::COUNTDOWN || ph == Phase::GAMEOVER);
            std::lock_guard<std::mutex> gc(clientMutex);
            SlotMask claimed = gatherClaimedSlots();
            refreshBotSlots(claimed, allowBotify);
            HandleMidMatchLeavers(claimed, allowBotify, TICK_DT);
        }

        //MARK: Tick sim
        // Keep simulating through PLAYING *and* GAMEOVER; only LOBBY (no world
        // yet) is idle. The GAMEOVER sim keeps the world moving during the
        // client's game-over countdown so networked play matches local, where
        // the sim runs every frame of that countdown (issue #2). The match-end
        // detection below stays PLAYING-only so it fires exactly once.
        Phase phase = gamePhase.load();
        // Wind an ended match down instead of simulating it forever. Both
        // thresholds are far past GAME_OVER_TIMER (5 s), so a client still
        // running its death-FX countdown sees exactly what it always did.
        bool simThisTick = (phase == Phase::PLAYING);
        if (phase == Phase::GAMEOVER && gameOverStamped) {
            double since = Duration(now - gameOverAt).count();
            simThisTick = since < GAMEOVER_SIM_SECONDS;
            if (!simThisTick && !gameOverSimIdle) {
                gameOverSimIdle = true;
                std::cout << "Match sim idle after " << (int)GAMEOVER_SIM_SECONDS
                          << "s in GAMEOVER (world kept, still broadcasting)\n";
            }
            if (since >= GAMEOVER_LOBBY_SECONDS) {
                // Nobody is watching: drop the world and go back to being an
                // empty lobby, exactly as at boot. Frees the platform/asteroid
                // vectors, and the grid's cells age out on their own sweep.
                gameSpace.clear();
                gameSpace.spawnPlayers();
                rosterSize.store((int)gameSpace.getPlayers().size());
                // A fresh lineup for the next match (#102). HERE rather than at
                // match start, because the lobby previews the bots that will fill
                // it - re-rolling at start would make that preview wrong by
                // exactly one match. The names then hold steady for the whole of
                // the next one, which matters because refreshBotSlots runs sixty
                // times a second.
                botNameOrder = ShuffledIndices(BOT_NAME_COUNT);
                rebuildWelcomeStatic();
                gamePhase       = Phase::LOBBY;
                gameOverStamped = false;
                gameOverSimIdle = false;
                phase           = Phase::LOBBY;
                simThisTick     = false;
                std::cout << "Match world freed after " << (int)GAMEOVER_LOBBY_SECONDS
                          << "s in GAMEOVER; back to lobby\n";
            }
        }
        if (simThisTick) {
            // Apply each client's latest input to their player slot
            {
                std::lock_guard<std::mutex> gc(clientMutex);
                // First PLAYING tick of a match: every client's latch still holds
                // the ABSOLUTE yaw/pitch from the END of the previous match -
                // clients send nothing while on the TITLE/COUNTDOWN/GAME_OVER
                // screens, so lastInput froze there and hasInput stayed true.
                // ApplyInputToPlayer treats lookDelta as an aim TARGET, so
                // applying it here would overwrite the face-the-centre spawn
                // orientation resetPlayersForMatch() just set - and this tick's
                // broadcast is exactly what the client seeds its own predYaw
                // from, latching the wrong aim for the whole match. That's why
                // only match 1 ever spawned facing centre (hasInput was false).
                // Cleared under the SAME clientMutex the apply below holds, not
                // at the phase flip, so a straggler packet from a client still
                // running its game-over countdown can't slip in between.
                // hasInput=false is the load-bearing part: it gates the loop
                // below AND makes the input handler accept the next packet
                // whatever its seq (see the `|| !hasInput` clause there).
                // lastSeq is deliberately left alone - the client's inputSeq is
                // monotonic across matches.
                if (wentLive) {
                    for (auto& [cid, client] : clients) {
                        client.hasInput    = false;
                        client.lastInput   = PlayerInput{};
                        client.firePending = false;
                    }
                }
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
                    ApplyInputToPlayer(player, input, TICK_DT, gravity, gameSpace);
                }
            }

            // Drive every bot slot (unoccupied slots) through the behaviour
            // tree, straight into ApplyPlayerInput - NOT ApplyInputToPlayer,
            // whose absolute-aim conversion is only for network clients. Bots
            // emit per-frame deltas exactly like the local client's drive loop.
            botController.drive(gameSpace, TICK_DT);

            gameSpace.updatePositions(TICK_DT);
            RunCollisionChecks(gameSpace, scratchGrid, &gridMs);
            gameSpace.updateActiveObjects();

            //MARK: Match end
            // End the match when EITHER every human is dead OR only one player
            // (human or bot) is left standing - so a 2-human match keeps going
            // past the first human's death, and both humans reach GAME OVER on
            // the same phase flip. The single-survivor clause is gated to
            // multi-PARTICIPANT rosters so a solo start doesn't end instantly;
            // it then ends only when the lone human dies (aliveHumans == 0),
            // preserving today's solo behavior.
            //
            // Participants, not players.size(): a room whose preset caps maxBots
            // can hold empty slots, and counting those as bodies made a one-human
            // match satisfy "only one left standing" on its very first PLAYING
            // tick. The slots are in the roster and joinable - they just aren't
            // anybody. A dead human keeps spectating
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
                int aliveTotal = 0, participants = 0;
                for (const auto& p : players) {
                    if (p.isAlive) aliveTotal++;
                    if (!p.isVacant) participants++;
                }
                bool multi = participants >= 2;
                if (aliveHumans == 0 || (multi && aliveTotal <= 1)) {
                    gamePhase = Phase::GAMEOVER;
                    std::cout << "Match over (humans alive " << aliveHumans
                              << ", total alive " << aliveTotal << ")\n";
                }
            }
        }

        //MARK: Scoreboard credit
        // One-shot on the PLAYING -> GAMEOVER edge. Detected here rather than at
        // either site that sets GAMEOVER, because those two hold different locks:
        // the host's endmatch handler runs on a session strand with no gameMutex,
        // while last-player-standing (just above) runs here holding it. Watching
        // the edge from the sim loop gives one site and one lock discipline, and
        // catches both causes.
        //
        // Timing is load-bearing: a match start calls generate(), which zeroes
        // every player's score, so the credit has to happen before the next one.
        {
            Phase nowPhase = gamePhase.load();
            if (prevPhase == Phase::PLAYING && nowPhase == Phase::GAMEOVER) {
                // Same edge the credit uses, so the wind-down clock below has one
                // origin however the match ended (host request or last player).
                gameOverAt      = now;
                gameOverStamped = true;
                std::lock_guard<std::mutex> gc(clientMutex);
                SlotMask claimed = gatherClaimedSlots();
                auto& ps = gameSpace.getPlayers();
                // Which identity each slot belongs to. Read from `clients`
                // (still locked above) rather than from the player, because
                // identity is a property of the CONNECTION - the body in slot 3
                // is only whoever is currently sitting in it.
                std::map<int, std::string> slotIdentity;
                for (const auto& [cid, c] : clients)
                    if (c.playerId >= 0) slotIdentity[c.playerId] = c.identity;

                std::lock_guard<std::mutex> sb(scoreboardMutex);
                for (int i = 0; i < (int)ps.size(); ++i) {
                    // Same "active" rule the state packet uses (see buildState):
                    // a slot a client holds, or one a bot is driving. Player's
                    // isConnected is CLIENT-side state - the server never sets it,
                    // so testing it here would filter nothing. Unoccupied slots
                    // still carry a name and a zero score, and would otherwise
                    // litter the table with 0-point entries every match.
                    if (!SlotSet(claimed, i) && !ps[i].isBot) continue;

                    // The row's KEY (D4). A bot has no identity to be issued, so
                    // it is keyed on its name behind a '-'; a human is keyed on
                    // the identity the server signed for them, which is what
                    // stops two players called MIKE sharing a row.
                    std::string id;
                    if (ps[i].isBot) {
                        id = BotIdFor(ps[i].name);
                    } else {
                        auto it = slotIdentity.find(i);
                        if (it != slotIdentity.end()) id = it->second;
                    }
                    // No identity means no row, and that is deliberate: the
                    // alternative is inventing a key, which is precisely the
                    // display-name guess D4 removed. A connection always has one
                    // by this point (both connect paths establish it), so this
                    // is a guard rather than a path.
                    if (id.empty()) continue;
                    scoreboard.addScore(id, ps[i].name, ps[i].score);

                    // And the RUN (D5). Same credit, a different question: the
                    // career row is a running total that belongs to a person,
                    // this is one historical event.
                    RunRow r;
                    r.score     = ps[i].score;
                    // WALL time, and the only wall clock in the server. Steady
                    // time has no anchor to a calendar - its zero is boot - so a
                    // date is the one thing it cannot give. Safe because this is
                    // display-only: a stepped clock puts a wrong date on a
                    // leaderboard row, where the same mistake in the reap timer
                    // would tear down live matches. Stored from day one even
                    // though nothing shows it, because a row written without one
                    // can never acquire it.
                    r.when      = (long long)std::chrono::system_clock::to_time_t(
                                      std::chrono::system_clock::now());
                    r.map       = MapSizeName(pendingMap.load());
                    r.official  = (matchKind == MatchKind::Official);
                    r.matchName = matchName;
                    // Every bot shares ONE identity here, unlike the career table
                    // where each keeps its own row. Nine bot names at three rows
                    // each would be 27 entries competing for a board of ten and
                    // would leave no room for humans at all; shared and capped at
                    // one, they contribute a single line - the best a bot has
                    // managed. The row still displays the bot that set it.
                    r.id        = ps[i].isBot ? RUN_BOT_ID : id;
                    // FROZEN. Renaming yourself does not rewrite what happened
                    // that Tuesday - the opposite of the career row above, which
                    // follows renames because a total belongs to a person.
                    r.name      = ps[i].name;
                    scoreboard.addRun(std::move(r));
                }
                scoreboard.generateLeaderboard();
                // NOT saved here. With several matches running, their ends bunch
                // up - and each save rewrites the whole file. The driver loop
                // flushes a dirty table on a timer instead (see SCORES_FLUSH_SEC).
                scoreboardDirty = true;
                leaderboardDirty = true;
                std::cout << "Scoreboard: credited match, " << scoreboard.scores.size()
                          << " careers / " << scoreboard.runs.size() << " runs total\n";
            }
            prevPhase = nowPhase;
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

    // Match just credited: push the updated table to everyone. Built before
    // taking clientMutex so scoreboardMutex is already released (lock order).
    // Off gameMutex, like the welcome resend above and BroadcastState below.
    if (leaderboardDirty) {
        // One message each now, not one broadcast: every client's pinned row is
        // its own. Eleven rows apiece, once per match end.
        std::lock_guard<std::mutex> gc(clientMutex);
        for (auto& [cid, client] : clients)
            SendToClient(client, buildLeaderboard(client.identity));
    }

    if (gridMs > 0.0) statGrid.Add(gridMs);

    // Broadcast authoritative state to all clients every tick.
    // At 60Hz this is ~60 packets/sec per client. For 2 players the
    // bandwidth is trivial; revisit delta-compression if player count grows.
    { ScopedTime _bc(statBroadcast); BroadcastState(tick); }

    // The heartbeat used to live here, one line per second per match. With N
    // matches that is N lines a second, and - worse - the Actions idle watchdog
    // greps the LAST "players N" in the log, so it would have been reading one
    // arbitrary match's count instead of the total. It now prints once from the
    // driver loop with real totals; see ReportHeartbeat.
    lastAsteroidCount = asteroidCount;
}

//MARK: Heartbeat
// One line per second proving the sim is alive, plus a per-match roll-call less
// often.
//
// FORMAT IS LOAD-BEARING. .github/workflows/gameserver.yml shuts an idle server
// down by grepping the LAST `players <n>` in the log:
//
//     players=$(grep -oE 'players [0-9]+' server.log | tail -1 | awk '{print $2}')
//
// So exactly one line per interval may carry that pattern, and it has to be the
// total across every match. The per-match lines below deliberately say "slots
// 3/8" rather than "players 3" - the regex would otherwise match them, `tail -1`
// would pick whichever printed last, and the watchdog would silently start
// reading one arbitrary room's population instead of the server's.
const int HEARTBEAT_ROLLCALL_SECONDS = 10;

static void ReportHeartbeat(uint32_t tick, int secondsElapsed) {
    const MatchRegistry::Totals t = g_registry.Summarise();

    // Worst p95 across matches - the number that decides whether the box is
    // keeping up, since a sequential scheduler is only as good as its slowest room.
    double worst = 0.0;
    for (const MatchListing& row : g_registry.List(/*includePrivate*/ true)) {
        if (auto m = g_registry.Find(row.code))
            worst = std::max(worst, m->statSim.Summarise().p95 + m->statBroadcast.Summarise().p95);
    }

    std::cout << "tick " << tick
              << "  matches " << t.matches << " (" << t.active << " active)"
              << "  players " << t.players           // <- the watchdog reads this
              << "  worst " << std::fixed << std::setprecision(2) << worst << "ms"
              << std::endl;

    if (secondsElapsed % HEARTBEAT_ROLLCALL_SECONDS != 0) return;
    for (const MatchListing& row : g_registry.List(true)) {
        auto m = g_registry.Find(row.code);
        if (!m) continue;
        std::cout << "    " << row.code << "  " << phaseString(row.phase)
                  << "  slots " << row.players << "/" << row.maxPlayers
                  << "  asteroids " << m->lastAsteroidCount
                  << (row.isPrivate ? "  private" : "")
                  << std::endl;
    }
}

//MARK: Sim loop
// -------------------------------------------------------------------------
// The 60 Hz driver. Sleeps to the next beat, then ticks the match. Today that
// is one call; A4 makes it a loop over every live match, which is why the tick
// body moved into Match::Tick above.
// -------------------------------------------------------------------------
void SimulationLoop() {
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::duration<double>;
    auto lastTick  = Clock::now();
    g_defaultMatch->prevPhase = g_defaultMatch->gamePhase.load();
    // One scratch grid for every match this thread drives - see the note in
    // match.h. Lives here (not in Match) so it stays warm across matches, and so
    // a future worker pool gets one per worker for free.
    CollisionGrid scratchGrid;
    int beat = 0;      // ticks since the last registry sweep
    int perfBeat = 0;  // seconds since the last PERF line
    int scoresBeat = 0; // seconds since the last scoreboard flush

    while (true) {
        auto now     = Clock::now();
        auto elapsed = Duration(now - lastTick).count();
        if (elapsed < TICK_DT) {
            std::this_thread::sleep_for(std::chrono::microseconds(
                (int)((TICK_DT - elapsed) * 900000)));
            continue;
        }
        lastTick = now;

        // EVERY match, not just the default one. Rooms created at runtime were
        // being routed to correctly and then never simulated, so a match in one
        // could be started and would simply sit in the lobby forever - the join
        // worked and the game never began.
        //
        // Sequential and single-threaded, which is what A4's measurements chose:
        // ~0.6 ms per full match against a 10 ms budget. It also means all these
        // matches share one warm scratch grid (see match.h), which a worker pool
        // could not do.
        for (auto& m : g_registry.All()) m->Tick(scratchGrid);

        // Registry upkeep, once a second rather than per tick - destroying rooms
        // is not something 60 Hz buys anything. The default room is pinned, so
        // today this only ever logs nothing.
        if (++beat >= (int)TICK_RATE) {
            beat = 0;
            ++g_uptimeSeconds;
            for (const std::string& code : g_registry.Reap(now))
                std::cout << "Match " << code << " reaped\n";
            SweepUnseated();

            // Flush the scoreboard if a match credited into it recently. Once,
            // here, however many matches ended - which is the whole point of
            // debouncing it rather than saving inside the credit.
            if (++scoresBeat >= SCORES_FLUSH_SEC) {
                scoresBeat = 0;
                if (scoreboardDirty.exchange(false)) {
                    std::lock_guard<std::mutex> sb(scoreboardMutex);
                    if (!scoreboard.save())
                        scoreboardDirty = true;   // try again next flush
                }
            }
            ReportHeartbeat(g_defaultMatch->serverTick.load(), g_uptimeSeconds.load());
            if (PerfEnabled() && ++perfBeat >= PERF_REPORT_SECONDS) {
                perfBeat = 0;
                ReportPerf(*g_defaultMatch, now);
            }
        }

        // Asked to stop (SIGTERM from `systemctl restart`, or Ctrl-C). Write out
        // anything the debounce is still holding, then leave. Checked every tick
        // rather than every second so a restart is not waiting on this.
        if (g_shutdown.load(std::memory_order_relaxed)) {
            if (scoreboardDirty.exchange(false)) {
                std::lock_guard<std::mutex> sb(scoreboardMutex);
                std::cout << "Shutting down: flushing the scoreboard\n";
                scoreboard.save();
            }
            std::cout << "Server stopped after " << g_uptimeSeconds.load() << "s\n";
            std::exit(0);
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
            std::lock_guard<std::mutex> lock(g_connMutex);
            auto idx = g_udpIndex.find(from);
            if (idx != g_udpIndex.end()) { connId = idx->second; known = true; }
        }
        if (known) { HandleClientMessage(connId, msg); return; }
        // Unknown endpoint: only a hello is looked at, and even that one gets
        // nothing but a cookie until it proves it can receive replies. Every
        // other verb - list, join, create, quick - is dropped unread, which is
        // E1 scope item 2: nothing large ever leaves here for an address we have
        // not confirmed is real. That falls out of this early return, so do not
        // "helpfully" start handling a directory request from a stranger.
        if (msg.find("\"type\":\"hello\"") != std::string::npos) RegisterPeer(from, msg);
    }

    void RegisterPeer(const udp::endpoint& from, const std::string& helloMsg) {
        // Join gate: a hello without the right key claims nothing and gets NO
        // reply - not even a cookie. To a port scanner a silent port looks like
        // nothing worth probing.
        if (!JoinKeyOk(parseString(helloMsg, "key"))) {
            std::cout << "UDP join rejected (bad key)\n";
            return;
        }
        // Return-routability check (E1). A first hello carries no cookie, so it
        // gets one back and nothing else; the client echoes it and arrives here
        // again, this time with something only an endpoint that actually
        // RECEIVED our reply could know. A spoofer's cookie went to the address
        // they forged, so they never get past this line - and the 51 bytes they
        // did extract cost them more to send than they got back.
        //
        // A stale cookie (older than two buckets) lands here too and is simply
        // re-challenged, which is also what a client coming back after a long
        // pause looks like. Nothing to distinguish, nothing to log.
        const std::string cookie = parseString(helloMsg, "c");
        if (!CookieOk(cookie, from)) {
            ConnectedClient probe;
            probe.transport   = Transport::UDP;
            probe.udpEndpoint = from;
            SendToClient(probe, buildChallenge(MintCookie(from, CookieBucketNow())));
            return;
        }
        // A hello may name the room it wants (that is how an invite link works over
        // UDP, mirroring ?match= on the WebSocket side). Naming nothing is the
        // ordinary case and means exactly that - SeatOrPark parks it below and
        // the player picks from the directory.
        //
        // This used to substitute the landing room here, which is subtle and was
        // the LAST thing forcing a seat: it rewrote the code before SeatOrPark
        // could see it was empty, so parking on an empty code looked correct and
        // did nothing on the transport most clients use.
        const std::string targetCode = clampName(parseString(helloMsg, "match"));
        auto target = targetCode.empty() ? nullptr : g_registry.Find(targetCode);

        // "cid" is the client's install id (D1). This is the path that matters
        // most for it: UDP has no disconnect event, so a client whose NAT mapping
        // changed across a laptop sleep arrives here as a total stranger on a new
        // endpoint - the id is the only thing tying it to the body still drifting
        // in the arena.
        const std::string cid = clampClientId(parseString(helloMsg, "cid"));
        // "tok" is the identity token (D3). Read here, on the hello, because
        // that is this transport's handshake - there is no URL on the wire.
        const Identity identity = EstablishIdentity(parseString(helloMsg, "tok"));
        // Only if they named a room that exists: superseding a stale twin is a
        // question about a specific roster, and there is no roster to ask about
        // when no room was named.
        if (target) {
            // Lock order gameMutex->clientMutex, matching Session::Accept.
            std::lock_guard<std::mutex> gg(target->gameMutex);
            std::lock_guard<std::mutex> gc(target->clientMutex);
            // Reap eagerly, not just on the periodic tick: a client reconnecting
            // under a new source endpoint (e.g. after its laptop slept and WiFi
            // got a new NAT mapping) must reclaim its old slot immediately
            // rather than racing this tick's scheduled sweep and losing to it.
            target->ReapIdleUdpClients();
            // If this player's previous connection is still sitting on their slot
            // but has gone quiet, let go of it. BEFORE the seating attempt below,
            // and in the room they asked for: their own stale twin is a common
            // reason that room looks full, and superseding it is what turns
            // "sorry, full" back into "here is your body".
            target->SupersedeStaleTwin(cid, from);
        }

        const uint64_t connId = nextConnId++;
        ConnectedClient c;
        c.clientId    = cid;
        c.identity    = identity.id;
        c.transport   = Transport::UDP;
        c.udpEndpoint = from;
        c.remoteAddr  = from.address().to_string();
        c.lastSeenSec = NowSec();
        const std::string nm = clampName(parseString(helloMsg, "name"));
        if (!nm.empty()) { c.name = nm; c.nameDirty = true; }

        // Route the endpoint FIRST, so whatever SeatOrPark decides, this peer's
        // next datagram already finds its connection. The receive loop has a
        // single outstanding read, so nothing can arrive in between.
        { std::lock_guard<std::mutex> cl(g_connMutex); g_udpIndex[from] = connId; }

        // Before the welcome, so a client that is about to be parked with no
        // slot still ends up with an identity for next time. The address is
        // already proved by the cookie above, so this is not a reply to a
        // stranger.
        if (!identity.issue.empty()) SendToClient(c, buildIdentity(identity.issue));

        // Seated, bumped to the default room, or parked with no slot at all -
        // one shared answer with the WS path, and none of them hang up. The old
        // code's last branch sent a "full" packet and simply never registered the
        // peer, so the client re-helloed into silence forever.
        SeatOrPark(connId, c, targetCode);
    }
};

//MARK: main
// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------
int main() {
    // Before anything prints or binds: the banner reports the port, so reading
    // this later made an overridden instance announce the default it was not
    // using.
    if (const char* pp = std::getenv("PLATFORMZ_PORT"); pp && *pp) {
        int v = std::atoi(pp);
        if (v > 0 && v < 65536) PORT = (unsigned short)v;
        else std::cerr << "PLATFORMZ_PORT=" << pp << " is not a usable port; keeping "
                       << PORT_DEFAULT << "\n";
    }

    // Catch the stop signals so the sim loop can flush the scoreboard on the way
    // out. Installed before anything binds, so a server killed during startup
    // still leaves cleanly.
    std::signal(SIGTERM, OnTerminate);
    std::signal(SIGINT,  OnTerminate);

    std::cout << "PLATFORMZ server | port " << PORT
              << (PORT == PORT_DEFAULT ? "" : " (PLATFORMZ_PORT override)")
              << " (TCP/WebSocket + UDP) | " << TICK_RATE << " Hz\n";
    // Protocol identity, so `journalctl -u platformz` answers "is the running
    // binary actually the one I just deployed?". That question is why a stale
    // server once went unnoticed: make didn't track game headers, so a
    // header-only pull rebuilt nothing and the restart swapped in the same
    // binary. Clients must match these exactly - see netbin.h.
    std::cout << "Protocol: state tag 0x" << std::hex << std::setfill('0')
              << std::setw(2) << (int)nb::STATE_BIN_VERSION
              << ", welcome tag 0x" << std::setw(2) << (int)nb::WELCOME_BIN_VERSION
              << std::dec << std::setfill(' ')
              << " | qpos +/-" << nb::QPOS_RANGE
              << " | qvel +/-" << nb::QVEL_RANGE << "\n";

    // Join key gate (never printed - it's the secret). Lives in the
    // environment, not the repo: docs/deploy-vultr.md covers setting it on
    // the box; friends receive it inside their invite link / handout build.
    // A4 instrumentation, off unless asked for (see server/perf.h).
    if (const char* pf = std::getenv("PLATFORMZ_PERF"); pf && *pf && std::string(pf) != "0") {
        PerfEnabled() = true;
        std::cout << "Perf sampling: ON (PERF line every " << PERF_REPORT_SECONDS << "s)\n";
    }

    if (const char* k = std::getenv("PLATFORMZ_KEY"); k && *k) {
        joinKey = k;
        std::cout << "Join key: REQUIRED (PLATFORMZ_KEY is set)\n";
    } else {
        std::cout << "Join key: none (open server; set PLATFORMZ_KEY to require one)\n";
    }

    // Live-match cap (E2). Off at the default - see MATCH_MAX_ACTIVE_DEFAULT in
    // constants.h - and there for the day the transfer graph says otherwise.
    if (const char* ma = std::getenv("PLATFORMZ_MAX_ACTIVE"); ma && *ma) {
        const int v = std::atoi(ma);
        if (v > 0 && v <= MATCH_MAX_CONCURRENT) g_maxActiveMatches = v;
        else std::cerr << "PLATFORMZ_MAX_ACTIVE=" << ma << " is not in 1.."
                       << MATCH_MAX_CONCURRENT << "; keeping "
                       << g_maxActiveMatches << "\n";
    }
    // Rooms one source address may mint. Raise it (or 0 to disable) when your
    // players share a NAT - see AllowCreate.
    if (const char* rp = std::getenv("PLATFORMZ_MAX_ROOMS_PER_ADDR"); rp && *rp) {
        const int v = std::atoi(rp);
        if (v >= 0 && v <= MATCH_MAX_CONCURRENT) g_maxRoomsPerAddr = v;
        else std::cerr << "PLATFORMZ_MAX_ROOMS_PER_ADDR=" << rp << " is not in 0.."
                       << MATCH_MAX_CONCURRENT << "; keeping "
                       << g_maxRoomsPerAddr << "\n";
    }
    if (const char* oo = std::getenv("PLATFORMZ_SCORES_OFFICIAL_ONLY");
        oo && *oo && std::string(oo) != "0") {
        g_scoresOfficialOnly = true;
        std::cout << "Leaderboard: official matches only"
                  << " (PLATFORMZ_SCORES_OFFICIAL_ONLY)\n";
    }

    std::cout << "Match caps: " << MATCH_MAX_CONCURRENT << " rooms, "
              << g_maxActiveMatches << " live"
              << (g_maxActiveMatches >= MATCH_MAX_CONCURRENT
                    ? " (no live cap; set PLATFORMZ_MAX_ACTIVE to add one)" : "")
              << ", " << (g_maxRoomsPerAddr > 0 ? std::to_string(g_maxRoomsPerAddr)
                                                : std::string("unlimited"))
              << " per address\n";

    // The key behind every tag this server issues - today E1's UDP handshake
    // cookie, later D3's identity token. Loaded before either listener exists,
    // because the first datagram to arrive needs it.
    {
        std::string warn;
        g_identitySecret = pz::LoadServerSecret(warn);
        std::cout << "UDP handshake cookie: ON (HMAC-SHA256, "
                  << (int)COOKIE_BUCKET_SEC << "s buckets)\n";
        if (!warn.empty()) std::cout << "WARNING: " << warn << "\n";
    }

    // Cumulative all-time scores. The default path is RELATIVE, so a dev build
    // keeps its scores beside the binary. That default does NOT work under the
    // systemd unit: DynamicUser=yes cannot write into the root-owned repo at
    // WorkingDirectory. The box therefore sets PLATFORMZ_SCORES to a path inside
    // its StateDirectory (/var/lib/platformz) - see docs/deploy-vultr.md. A
    // missing file is a normal cold start; a failing WRITE is not, so the boot
    // log below prints the path in use.
    {
        const char* sp = std::getenv("PLATFORMZ_SCORES");
        std::lock_guard<std::mutex> lk(scoreboardMutex);
        scoreboard.setFilePath((sp && *sp) ? sp : SCOREBOARD_FILEPATH);
        scoreboard.load();
        scoreboard.generateLeaderboard(); // seed the table sent to clients on join
        std::cout << "Scoreboard: " << scoreboard.scores.size() << " careers, "
                  << scoreboard.runs.size() << " runs from "
                  << scoreboard.filePath << "\n";
    }

    // One resident OFFICIAL room per preset, pinned so it is always there.
    //
    // Answering A2's open question the expensive-looking way, because the cheap
    // way is worse: a room created only on demand means the browser is empty
    // until somebody asks for one, and "NO MATCHES YET" is the first thing a new
    // player sees in a game whose whole premise is finding a match. An idle LOBBY
    // room costs almost nothing to tick - A1b's work is what makes this
    // affordable - and it is what QUICK MATCH lands in, so the common path never
    // pays for creation at all.
    //
    // One per preset: adding CHAOS to matchOptionPresets adds its room here with
    // no code change, which is the point of that table being data.
    for (const auto& [presetName, preset] : matchOptionPresets) {
        MatchRegistry::CreateResult why;
        std::string code;
        auto m = g_registry.Create(preset.label, presetName,
                                   MatchKind::Official, /*isPrivate*/ false,
                                   /*joinCode*/ "", code, why);
        if (!m) { std::cerr << "could not create the official " << presetName << " room\n"; return 1; }
        PrimeLobby(*m);
        g_registry.Pin(code);
        std::cout << "Match registry: official room " << code
                  << " preset=" << presetName
                  << " (locked, auto-starts at " << m->pendingMinHumans.load()
                  << " players, maxBots=" << m->pendingMaxBots.load() << ")\n";

        // The FIRST preset is the landing room: where a connection goes when it
        // names no room, where `leave` returns you, and the fallback when a
        // requested room is gone or full.
        //
        // There used to be a separate CUSTOM room called PLATFORMZ for this, back
        // when a client could not pick a room at all and needed somewhere host-run
        // to press START in. The client browses and picks now, so that room was a
        // placeholder sitting in everyone's match list with nothing behind it.
        // matchOptionPresets is ordered front-door-first for exactly this reason -
        // it is already what QUICK MATCH hands a stranger.
        //
        // Consequence worth knowing: the landing room is OFFICIAL, so it is locked
        // and starts itself. Nobody hosts it and nobody presses START there; a
        // player who wants to run their own rules creates a custom room.
        if (!g_defaultMatch) { g_defaultMatch = m; g_defaultCode = code; }
    }
    // Every fallback path below dereferences this, so an empty preset table is a
    // boot failure rather than a null waiting to be hit by the first connection.
    if (!g_defaultMatch) { std::cerr << "no presets: there is no room to land in\n"; return 1; }
    std::cout << "Match registry: landing room " << g_defaultCode
              << " (cap " << MATCH_MAX_CONCURRENT << ")\n";
    // BOOT-READY LINE. "lobby ready" is what run_probes.sh and ci_smoke.sh wait
    // for before they start talking to the server, so keep that substring even if
    // the rest of the sentence changes - four consumers grep for it (the two
    // runners and both testing docs). It means the same thing it always did: the
    // landing room has its slots and its cached welcome, so a client can connect.
    std::cout << "GameSpace: lobby ready, "
              << g_defaultMatch->gameSpace.getPlayers().size()
              << " player slots (official room - it starts itself)\n";

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
