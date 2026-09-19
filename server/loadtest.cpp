// server/loadtest.cpp
//
// Headless load generator and protocol smoke test for the PLATFORMZ server (E3).
//
//   make -C server loadtest
//   server/loadtest --matches 12 --clients 8 --seconds 60 --map MEDIUM
//   server/loadtest --mode ws-smoke                    # one WebSocket client
//
// WHY THIS EXISTS. `MATCH_MAX_CONCURRENT = 12` and the ~310 KB/s-per-match egress
// figure behind it (A4, docs/perf-measurements.md) were derived from ONE match
// measured on the box and arithmetic. Nothing has ever put twelve of them under
// real load at once, which means the number the whole capacity story rests on
// has never been observed. This observes it.
//
// It also drives the paths only a crowd reaches: E1's handshake cookie at
// ninety-odd simultaneous handshakes, E2's per-address and move budgets, chunked
// welcomes on the big maps, and the sim loop ticking every room in one 60 Hz beat.
//
// WHAT IT MEASURES, AND WHAT IT DOES NOT. From out here we can see what a client
// sees: state packets arriving, their rate, the gaps between them, and bytes on
// the wire. We CANNOT see tick times - those are the server's own, and it already
// reports them (PLATFORMZ_PERF=1, see server/perf.h). Run both and read them
// together; this tool deliberately does not invent a number it cannot measure.
//
//   # on the box                          # from ANOTHER machine
//   PLATFORMZ_PERF=1 ./gameserver         ./loadtest --host the.box --matches 12
//
// Running the generator on the box measures the generator competing with the
// thing it is measuring, which is the one way to get numbers that mean nothing.
//
// UDP, NOT WEBSOCKET, for the load half. The issue asked for WS; the numbers
// that matter are UDP's, because that is what the native client speaks, it is
// where the binary state packet and the chunked welcome live, and E1's cookie
// exists only there. WebSocket still gets covered - as a smoke test (--mode
// ws-smoke), which is what CI runs, and which until now was the only client
// transport with no automated coverage at all.

#include "../wire.h"      // serializeHello / serializeInput / serializeStart - the REAL encoders
#include "../netbin.h"    // packet tags
#include "../constants.h" // MATCH_MAX_CONCURRENT (map presets moved to options.h)

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace beast     = boost::beast;
namespace websocket = boost::beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;
using Clock         = std::chrono::steady_clock;

static double NowSec() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

//MARK: Options
struct Opts {
    std::string host = "127.0.0.1";
    std::string port = "9000";
    std::string key;                 // PLATFORMZ_KEY, if the server has a join gate
    std::string map  = "MEDIUM";
    std::string mode = "load";       // load | ws-smoke
    int matches = 4;
    int clients = 4;                 // per match
    int seconds = 30;
    bool quiet  = false;
};

static void Usage() {
    std::cout <<
      "usage: loadtest [options]\n"
      "  --host H --port P     server to drive (default 127.0.0.1:9000)\n"
      "  --key K               join key, if the server requires one\n"
      "  --matches N           rooms to create and fill (default 4)\n"
      "  --clients N           clients per room (default 4)\n"
      "  --seconds N           how long to hold the load (default 30)\n"
      "  --map SMALL|MEDIUM|LARGE|XL\n"
      "  --mode load|ws-smoke  (default load)\n"
      "  --quiet               totals only\n";
}

//MARK: One UDP client
// A deliberately thin client: it speaks the real wire (wire.h builds every
// outbound message) but decodes only as far as it needs to - a tag byte and a
// slot. Decoding the world would mean a GameSpace per client, which at ninety-six
// clients would have this process measuring its own allocator.
struct Client {
    int         fd   = -1;
    int         slot = -1;         // from the welcome; -1 = not seated yet
    std::string name;
    std::string cookie;            // E1 handshake cookie, echoed in the next hello
    std::string matchCode;         // room we were welcomed into
    std::string wantMatch;         // room we are trying to reach

    uint32_t seq = 0;
    uint32_t epoch = 0;

    // What we saw. `states` is the honest measure of whether the server kept up:
    // it broadcasts at 60 Hz, so a client that got far fewer than 60*seconds
    // either lost datagrams or was starved.
    uint64_t states = 0, welcomes = 0, chunks = 0, others = 0, bytes = 0;
    double   firstStateSec = 0.0, lastStateSec = 0.0;
    double   worstGapSec = 0.0;
    int      challenges = 0;
    std::vector<std::string> refusals;   // joinfail reasons, in order

    bool Connect(const Opts& o) {
        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(o.host.c_str(), o.port.c_str(), &hints, &res) != 0 || !res) return false;
        fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd >= 0 && ::connect(fd, res->ai_addr, res->ai_addrlen) < 0) { ::close(fd); fd = -1; }
        freeaddrinfo(res);
        if (fd < 0) return false;
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        return true;
    }

    void Send(const std::string& s) const {
        if (fd >= 0) ::send(fd, s.data(), s.size(), 0);
    }

    void Hello(const Opts& o) {
        // clientId left empty: every client here is a distinct player, and a
        // shared install id would have them superseding each other's slots.
        Send(serializeHello(name, o.key, /*clientId*/ "", wantMatch, cookie));
    }

    void Close() { if (fd >= 0) { ::close(fd); fd = -1; } }

    // Drain everything waiting. Returns false only if the socket is gone.
    void Drain(const Opts& o) {
        if (fd < 0) return;
        char buf[65536];
        for (;;) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            bytes += (uint64_t)n;
            const uint8_t tag = (uint8_t)buf[0];
            if (tag == nb::STATE_BIN_VERSION) {
                const double t = NowSec();
                if (states == 0) firstStateSec = t;
                else worstGapSec = std::max(worstGapSec, t - lastStateSec);
                lastStateSec = t;
                states++;
                // The match epoch, so our input is not dropped as stale. Header
                // is u8 tag, u32 tick, u32 lastSeq, then the body: u8 phase,
                // f32 countdown, u32 epoch.
                if (n >= 18) std::memcpy(&epoch, buf + 14, 4);
            } else if (tag == nb::WELCOME_BIN_VERSION) {
                welcomes++;
                if (n >= 10) {
                    int32_t s = -1;
                    std::memcpy(&s, buf + 1, 4);
                    slot = s;
                    const uint8_t clen = (uint8_t)buf[9];
                    if (n >= 10 + clen) matchCode.assign(buf + 10, buf + 10 + clen);
                }
            } else if (tag == nb::CHUNK_VERSION) {
                // Counted, not reassembled: a chunk means the welcome for this
                // map did not fit one datagram, which is a fact worth reporting.
                chunks++;
            } else if (tag == '{') {
                others++;
                const std::string j(buf, buf + n);
                if (j.find("\"type\":\"challenge\"") != std::string::npos) {
                    // E1: prove we can receive at this address, then re-hello.
                    const auto k = j.find("\"c\":\"");
                    if (k != std::string::npos) {
                        const auto e = j.find('"', k + 5);
                        if (e != std::string::npos) cookie = j.substr(k + 5, e - k - 5);
                    }
                    challenges++;
                    Hello(o);
                } else if (j.find("\"type\":\"joinfail\"") != std::string::npos) {
                    const auto k = j.find("\"why\":\"");
                    const auto e = k == std::string::npos ? k : j.find('"', k + 7);
                    refusals.push_back(e == std::string::npos ? "?" : j.substr(k + 7, e - k - 7));
                } else if (j.find("\"type\":\"created\"") != std::string::npos) {
                    const auto k = j.find("\"m\":\"");
                    const auto e = k == std::string::npos ? k : j.find('"', k + 5);
                    if (e != std::string::npos) matchCode = j.substr(k + 5, e - k - 5);
                }
            } else {
                others++;
            }
        }
    }
};

//MARK: Percentiles
static double Pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = (size_t)std::min((double)v.size() - 1,
                                      std::max(0.0, p * (double)(v.size() - 1)));
    return v[i];
}

//MARK: WebSocket smoke test
// One connection, over the transport the browser build uses. Asserts the welcome
// arrives, that state packets follow, and that the identity handshake (D3)
// behaves - which is the whole contract a client depends on, and which had no
// automated coverage before this.
struct WsResult {
    bool        welcome = false;
    int         states  = 0;
    std::string identity;     // a token the server issued us, if it did
    std::string error;
};

// `token` is presented on the URL, exactly as the real client presents it.
static WsResult WsConnect(const Opts& o, const std::string& token, double seconds,
                          int wantStates) {
    WsResult r;
    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        websocket::stream<tcp::socket> ws(ioc);
        const auto results = resolver.resolve(o.host, o.port);
        net::connect(ws.next_layer(), results.begin(), results.end());
        // The join key and the identity token both ride the URL on this
        // transport - a WS client is welcomed the instant it connects, so the
        // handshake is the only moment guaranteed to happen.
        std::string target = "/";
        const char* sep = "?";
        if (!o.key.empty()) { target += sep + std::string("key=") + o.key; sep = "&"; }
        if (!token.empty()) { target += sep + std::string("tok=") + token; }
        ws.handshake(o.host + ":" + o.port, target);

        const double deadline = NowSec() + seconds;
        beast::flat_buffer buffer;
        while (NowSec() < deadline && (!r.welcome || r.states < wantStates)) {
            buffer.clear();
            ws.read(buffer);
            const std::string msg = beast::buffers_to_string(buffer.data());
            if (msg.find("\"type\":\"welcome\"") != std::string::npos) {
                r.welcome = true;
                ws.write(net::buffer(serializeName("SMOKE")));
            } else if (msg.find("\"type\":\"state\"") != std::string::npos) {
                r.states++;
            } else if (msg.find("\"type\":\"identity\"") != std::string::npos) {
                const auto k = msg.find("\"tok\":\"");
                const auto e = k == std::string::npos ? k : msg.find('"', k + 7);
                if (e != std::string::npos) r.identity = msg.substr(k + 7, e - k - 7);
            }
        }
        ws.close(websocket::close_code::normal);
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

static int WsSmoke(const Opts& o) {
    std::cout << "ws-smoke: " << o.host << ":" << o.port << "\n";
    int bad = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
        if (!ok) bad++;
    };

    const WsResult first = WsConnect(o, /*token*/ "", 10.0, 30);
    if (!first.error.empty()) { std::cout << "  FAIL websocket error: " << first.error << "\n"; return 1; }
    check(first.welcome, "a welcome arrived");
    check(first.states >= 30, "state packets stream (" + std::to_string(first.states) + " in 10s)");

    // D3 over WebSocket. The token rides the upgrade URL here rather than a
    // hello, so this is the only place that path is exercised at all.
    check(first.identity.size() == 64 &&
          first.identity.find_first_not_of("0123456789abcdef") == std::string::npos,
          "a client with no token was issued one (" +
          (first.identity.empty() ? std::string("none") : first.identity.substr(0, 12) + "...") + ")");

    if (!first.identity.empty()) {
        const WsResult again = WsConnect(o, first.identity, 6.0, 5);
        if (!again.error.empty()) { std::cout << "  FAIL reconnect: " << again.error << "\n"; return 1; }
        check(again.welcome, "reconnecting with that token still gets a welcome");
        check(again.identity.empty(),
              "...and NO new token, so the server recognised it");

        const WsResult bogus = WsConnect(o, std::string(64, 'z'), 6.0, 5);
        if (!bogus.error.empty()) { std::cout << "  FAIL bad-token connect: " << bogus.error << "\n"; return 1; }
        check(bogus.welcome, "a bad token is still welcomed - never a hard fail");
        check(!bogus.identity.empty() && bogus.identity != first.identity,
              "...and replaced with a fresh one");
    }

    if (bad) { std::cout << "ws-smoke FAILED\n"; return 1; }
    std::cout << "ws-smoke passed\n";
    return 0;
}

//MARK: Load run
static int Load(const Opts& o) {
    const auto preset = MatchPresetByName(o.map);
    const int total = o.matches * o.clients;
    std::cout << "load: " << o.matches << " matches x " << o.clients << " clients = "
              << total << " connections against " << o.host << ":" << o.port
              << ", map " << o.map << ", " << o.seconds << "s\n";
    if (o.matches > MATCH_MAX_CONCURRENT)
        std::cout << "note: --matches " << o.matches << " is above MATCH_MAX_CONCURRENT ("
                  << MATCH_MAX_CONCURRENT << "); the server will refuse the extras\n";

    std::vector<Client> cs((size_t)total);
    for (int i = 0; i < total; ++i) {
        cs[(size_t)i].name = "L" + std::to_string(i);
        if (!cs[(size_t)i].Connect(o)) {
            std::cout << "FAIL could not open a socket for client " << i << "\n";
            return 1;
        }
    }

    // --- Handshake -------------------------------------------------------
    // Everyone says hello at once, which is the interesting case for E1: the
    // server answers each with a cookie and seats nobody until it comes back.
    //
    // pumpFor also KEEPS CLIENTS ALIVE, which is not incidental. UDP has no
    // disconnect event, so the server culls a lobby client that has been silent
    // for UDP_CLIENT_TIMEOUT_LOBBY (3s) - and a client here is silent for whole
    // seconds at a time while rooms are being created and matches are counting
    // down. The real client sends a keepalive every second for exactly this
    // reason. Without it this harness watched the server evict and re-seat its
    // own clients, and reported the resulting mess as a server failure.
    auto pumpFor = [&](double secs) {
        const double until = NowSec() + secs;
        double nextPing = NowSec();
        while (NowSec() < until) {
            if (NowSec() >= nextPing) {
                // EVERY client, seated or parked. The server stamps liveness on
                // any message, and an unseated client is swept on the same 3s
                // silence rule a lobby client is - so gating this on `slot >= 0`
                // had the parked half culled while rooms were being created, and
                // their joins then failed for a reason that looked like the
                // server refusing them.
                for (Client& c : cs) c.Send(serializeKeepalive());
                nextPing += 0.5;
            }
            for (Client& c : cs) c.Drain(o);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    for (Client& c : cs) c.Hello(o);
    pumpFor(1.5);
    // UDP loses packets, so the real client re-sends hello until welcomed. Do
    // the same - but only a few rounds, and only while the server still has
    // room: past that a client is PARKED (E2) rather than lost, and re-helloing
    // a full server just collects `server_full` refusals. A parked client can
    // still create and join, which is the next thing we ask it to do.
    for (int round = 0; round < 4; ++round) {
        int missing = 0;
        for (Client& c : cs) if (c.slot < 0 && c.refusals.empty()) { c.Hello(o); missing++; }
        if (missing == 0) break;
        pumpFor(0.5);
    }
    int seated = 0, parked = 0, challenges = 0;
    for (const Client& c : cs) { (c.slot >= 0 ? seated : parked)++; challenges += c.challenges; }
    std::cout << "  " << seated << " seated, " << parked << " parked with no slot"
              << " (" << challenges << " cookie challenges)\n";
    if (seated == 0 && parked == 0) { std::cout << "FAIL nobody reached the server\n"; return 1; }

    // --- Spread across rooms ---------------------------------------------
    // One client per group creates a room; the rest join it by code. A parked
    // client can do this too - that is the point of E2 - so a server whose
    // default room is full is not a reason to stop here.
    //
    // Rooms are created from ONE address, so the server needs
    // PLATFORMZ_MAX_ROOMS_PER_ADDR raised past --matches, or E2's budget refuses
    // the extras. Reported below rather than silently under-provisioning.
    for (int m = 0; m < o.matches; ++m) {
        Client& host = cs[(size_t)(m * o.clients)];
        host.matchCode.clear();
        host.Send(serializeCreate("LOAD" + std::to_string(m), o.map, /*isPrivate*/ false));
        pumpFor(0.3);
    }
    pumpFor(1.0);

    int madeRooms = 0, refusedRooms = 0;
    for (int m = 0; m < o.matches; ++m)
        (cs[(size_t)(m * o.clients)].matchCode.empty() ? refusedRooms : madeRooms)++;
    std::cout << "  " << madeRooms << "/" << o.matches << " rooms created";
    if (refusedRooms)
        std::cout << " (" << refusedRooms << " refused - raise PLATFORMZ_MAX_ROOMS_PER_ADDR"
                  << " past " << o.matches << " on the server)";
    std::cout << "\n";
    if (madeRooms == 0) { std::cout << "FAIL no rooms to load\n"; return 1; }

    for (int m = 0; m < o.matches; ++m) {
        const std::string code = cs[(size_t)(m * o.clients)].matchCode;
        if (code.empty()) continue;
        for (int i = 1; i < o.clients; ++i) {
            cs[(size_t)(m * o.clients + i)].Send(serializeJoin(code));
            // Paced: E2 budgets room moves at 5 in hand and 1/s per connection.
            // Each client moves once so it cannot hit its own budget, but pacing
            // keeps the server's join path from being a thundering herd, which
            // is a different test than this one.
            pumpFor(0.15);
        }
    }
    pumpFor(1.0);

    int inRooms = 0;
    for (int m = 0; m < o.matches; ++m) {
        const std::string code = cs[(size_t)(m * o.clients)].matchCode;
        if (code.empty()) continue;
        for (int i = 0; i < o.clients; ++i)
            if (cs[(size_t)(m * o.clients + i)].matchCode == code) inRooms++;
    }
    std::cout << "  " << inRooms << " clients spread across " << madeRooms << " rooms\n";

    // --- Start every match ------------------------------------------------
    MatchOptions mo;
    mo.mapSize    = o.map;
    mo.numPlayers = GAMESPACE_NUMBER_OF_PLAYERS;   // full roster: bots fill the gaps
    for (int m = 0; m < o.matches; ++m) {
        Client& host = cs[(size_t)(m * o.clients)];
        if (host.matchCode.empty()) continue;
        host.Send(serializeStart(mo));
        pumpFor(0.1);
    }
    // Outlast COUNTDOWN_SECONDS so the measurement window is all PLAYING.
    std::cout << "  starting matches (countdown " << (int)COUNTDOWN_SECONDS << "s)...\n";
    pumpFor(COUNTDOWN_SECONDS + 2.0);

    // --- Hold the load ----------------------------------------------------
    for (Client& c : cs) { c.states = 0; c.bytes = 0; c.worstGapSec = 0.0; }
    const double t0 = NowSec();
    const double tEnd = t0 + o.seconds;
    uint64_t sent = 0, ticks = 0;
    double nextTick = t0;
    while (NowSec() < tEnd) {
        ticks++;
        // 60 Hz input from every seated client, which is what the real client
        // sends while PLAYING and what sizes the server's parse cost.
        PlayerInput in;
        in.moveAxis = { 0.4f, 1.0f };
        for (Client& c : cs) {
            if (c.slot < 0) continue;
            c.seq++;
            in.jetpack = (c.seq % 30) == 0;
            in.fire    = (c.seq % 8) == 0;
            c.Send(serializeInput(c.seq, in, -1.57f + (float)(c.seq % 90) * 0.02f, 0.0f, c.epoch));
            sent++;
        }
        for (Client& c : cs) c.Drain(o);
        nextTick += 1.0 / 60.0;
        const double sleep = nextTick - NowSec();
        if (sleep > 0) std::this_thread::sleep_for(std::chrono::duration<double>(sleep));
        else nextTick = NowSec();   // we are behind; do not spiral
    }
    pumpFor(0.5);
    const double held = NowSec() - t0;

    // --- Report -----------------------------------------------------------
    uint64_t states = 0, bytes = 0;
    std::vector<double> rates, gaps;
    int active = 0;
    for (const Client& c : cs) {
        if (c.slot < 0) continue;
        active++;
        states += c.states;
        bytes  += c.bytes;
        rates.push_back((double)c.states / held);
        gaps.push_back(c.worstGapSec);
    }
    const double expected = 60.0 * held;
    const double meanRate = active ? (double)states / active / held : 0.0;

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\n--- " << held << "s with " << active << " clients in "
              << madeRooms << " matches ---\n";
    std::cout << "  input sent      " << sent << " packets ("
              << (double)sent / held << "/s)\n";
    // If the generator itself could not keep 60 Hz, every number below is about
    // this machine and not about the server. Say so rather than letting someone
    // read a starved harness as a slow server.
    const double loopHz = (double)ticks / held;
    std::cout << "  generator ran   " << loopHz << " Hz"
              << (loopHz < 55.0 ? "  <-- BELOW 60: the harness is the bottleneck,"
                                  " not the server" : "") << "\n";
    std::cout << "  state received  " << states << " packets\n";
    std::cout << "  per client      " << meanRate << "/s vs 60/s expected  ("
              << std::setprecision(1) << (100.0 * meanRate / 60.0) << "% delivered)\n";
    std::cout << "  slowest client  " << Pct(rates, 0.0) << "/s\n";
    std::cout << std::setprecision(0);
    std::cout << "  worst gap       " << Pct(gaps, 0.5) * 1000.0 << " ms p50, "
              << Pct(gaps, 0.95) * 1000.0 << " ms p95, "
              << Pct(gaps, 1.0) * 1000.0 << " ms max\n";
    std::cout << std::setprecision(1);
    std::cout << "  egress seen     " << (double)bytes / held / 1024.0 << " KB/s total, "
              << (double)bytes / held / 1024.0 / std::max(1, madeRooms) << " KB/s per match\n";
    const double monthlyTB = (double)bytes / held * 2592000.0 / 1e12;
    std::cout << "  extrapolated    " << std::setprecision(2) << monthlyTB
              << " TB/month at this load (the $6 plan allows 2)\n";

    uint64_t chunks = 0, refusals = 0;
    for (const Client& c : cs) { chunks += c.chunks; refusals += c.refusals.size(); }
    if (chunks)   std::cout << "  chunked frames  " << chunks
                            << " (the " << o.map << " welcome does not fit one datagram)\n";
    if (refusals) std::cout << "  refusals        " << refusals << "\n";

    // The one hard pass/fail. Everything above is a measurement to read; this is
    // the claim: with this many matches the server still broadcasts to everyone
    // at something close to its tick rate. 80% leaves room for ordinary UDP loss
    // without excusing a server that has fallen behind.
    const bool ok = meanRate >= 0.80 * 60.0;
    std::cout << "\n" << (ok ? "PASS" : "FAIL") << " sustained "
              << std::setprecision(1) << (100.0 * meanRate / 60.0)
              << "% of the expected broadcast rate\n";
    if (!ok)
        std::cout << "  (read the server's own PERF line for tick cost - run it with"
                  << " PLATFORMZ_PERF=1)\n";

    for (Client& c : cs) { c.Send(serializeGoodbye()); c.Close(); }
    (void)expected;
    return ok ? 0 : 1;
}

//MARK: main
int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--host")    o.host = next();
        else if (a == "--port")    o.port = next();
        else if (a == "--key")     o.key = next();
        else if (a == "--map")     o.map = next();
        else if (a == "--mode")    o.mode = next();
        else if (a == "--matches") o.matches = std::atoi(next().c_str());
        else if (a == "--clients") o.clients = std::atoi(next().c_str());
        else if (a == "--seconds") o.seconds = std::atoi(next().c_str());
        else if (a == "--quiet")   o.quiet = true;
        else if (a == "-h" || a == "--help") { Usage(); return 0; }
        else { std::cout << "unknown option: " << a << "\n"; Usage(); return 2; }
    }
    if (const char* h = std::getenv("PLATFORMZ_HOST"); h && *h && o.host == "127.0.0.1") o.host = h;
    if (const char* k = std::getenv("PLATFORMZ_KEY");  k && *k && o.key.empty())         o.key = k;

    if (o.mode == "ws-smoke") return WsSmoke(o);
    if (o.mode != "load") { std::cout << "unknown --mode " << o.mode << "\n"; return 2; }
    if (mapSizePresets.find(o.map) == mapSizePresets.end()) {
        std::cout << "unknown --map " << o.map << "\n"; return 2;
    }
    if (o.matches < 1 || o.clients < 1 || o.seconds < 1) { Usage(); return 2; }
    return Load(o);
}
