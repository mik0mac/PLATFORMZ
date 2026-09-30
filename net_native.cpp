// net_native.cpp
//
// The desktop network transports behind net_client.h's ITransport: WebSocket
// (IXWebSocket) and raw UDP. Their own translation unit so that no platform
// socket header ever meets raylib.h - on Windows the two collide (see the note
// above MakeTransport in net_client.h). This file must never include raylib.h,
// nor any game header that does.
//
// The browser build compiles this file too (it is in the Makefile's SRCS) and
// gets nothing from it: its backend is the emscripten one in net_client.h.

#if !defined(__EMSCRIPTEN__)

#include "net_client.h"
#include "netbin.h"   // chunk framing constants (UDP reassembly in UdpTransport)

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>   // on Windows: winsock2.h, ws2tcpip.h, WSAStartup

#include <mutex>
#include <atomic>
#include <cstdint>

#if defined(_WIN32)
// Winsock. IXNetSystem.h above already brought in winsock2.h/ws2tcpip.h (after
// defining WIN32_LEAN_AND_MEAN); ws2_32 is linked by IXWebSocket's CMake target.
using SocketHandle = SOCKET;
static const SocketHandle BAD_SOCKET = INVALID_SOCKET;
static void CloseSocket(SocketHandle s)       { ::closesocket(s); }
static bool SetNonBlocking(SocketHandle s)    { u_long on = 1; return ::ioctlsocket(s, FIONBIO, &on) == 0; }
#else
// POSIX sockets (macOS/Linux). No extra link flags.
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
using SocketHandle = int;
static const SocketHandle BAD_SOCKET = -1;
static void CloseSocket(SocketHandle s)       { ::close(s); }
static bool SetNonBlocking(SocketHandle s) {
    const int flags = ::fcntl(s, F_GETFL, 0);
    return flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

// --- WebSocket transport (IXWebSocket) --------------------------------------
// Runs its own background thread and delivers messages through a callback; we
// push inbound text frames onto a mutex-guarded queue and drain it once per
// frame so game state is still only touched from the main thread.
class WsTransport : public ITransport {
public:
    WsTransport()  { ix::initNetSystem(); }
    ~WsTransport() override { ws_.stop(); ix::uninitNetSystem(); }

    // Begin connecting (non-blocking). IXWebSocket retries with backoff on its
    // own thread, so a server that isn't up yet - or a dropped connection -
    // recovers without any extra code here.
    void connect(const std::string& url) override {
        ws_.setUrl(url);
        // Keepalive: ping every 15s so an otherwise-idle connection isn't
        // dropped by a heartbeat/NAT idle timeout during long sessions.
        ws_.setPingInterval(15);
        ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Message: {
                    std::lock_guard<std::mutex> lk(mtx_);
                    inbox_.push_back(msg->str);
                    break;
                }
                case ix::WebSocketMessageType::Open:
                    open_.store(true);
                    break;
                case ix::WebSocketMessageType::Close:
                    open_.store(false);
                    break;
                case ix::WebSocketMessageType::Error: {
                    open_.store(false);
                    std::lock_guard<std::mutex> lk(mtx_);
                    lastError_ = msg->errorInfo.reason;
                    break;
                }
                default: break; // Ping/Pong/Fragment - ignored
            }
        });
        ws_.start();
    }

    void send(const std::string& s) override { ws_.send(s); }
    bool isOpen() const override { return open_.load(); }

    // Drain every queued inbound frame (oldest first). Called once per frame.
    std::vector<std::string> poll() override {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lk(mtx_);
        out.reserve(inbox_.size());
        while (!inbox_.empty()) { out.push_back(std::move(inbox_.front())); inbox_.pop_front(); }
        return out;
    }

    std::string lastError() override {
        std::lock_guard<std::mutex> lk(mtx_);
        return lastError_;
    }

    void stop() override { ws_.stop(); }

private:
    ix::WebSocket           ws_;
    std::mutex              mtx_;
    std::deque<std::string> inbox_;     // guarded by mtx_
    std::string             lastError_; // guarded by mtx_
    std::atomic<bool>       open_{false};
};

// --- UDP transport (raw datagrams) ------------------------------------------
// A single connected, non-blocking UDP socket to the server. "Connected" UDP
// means send/recv default to the server and recv ignores anything from other
// peers; it also surfaces ICMP port-unreachable (server down) as a recv error,
// which we simply ignore - the client keeps sending hello until the server
// answers. No background thread: poll() drains recv on the main thread.
//
// isOpen() flips true as soon as the socket exists. There is no transport-level
// handshake for UDP (that's the game-level hello/welcome, gated by `myIndex` in
// main.cpp) - this stays a dumb string pipe.
class UdpTransport : public ITransport {
public:
    // initNetSystem is WSAStartup on Windows (a no-op elsewhere), and it is
    // reference-counted, so pairing it per transport is safe. The WebSocket
    // transport always did this; UDP never needed to until Windows.
    UdpTransport()  { ix::initNetSystem(); }
    ~UdpTransport() override { stop(); ix::uninitNetSystem(); }

    void connect(const std::string& url) override {
        // url is "udp://host:port", optionally with a query ("?key=...") which
        // is not transport information - main.cpp reads it and puts the key in
        // the hello message instead. Strip it before parsing host:port.
        std::string hostport = url.substr(std::string("udp://").size());
        const auto query = hostport.find('?');
        if (query != std::string::npos) hostport = hostport.substr(0, query);
        const auto colon = hostport.rfind(':');
        if (colon == std::string::npos) { lastError_ = "udp url needs host:port"; return; }
        const std::string host = hostport.substr(0, colon);
        const std::string port = hostport.substr(colon + 1);

        addrinfo hints{};
        hints.ai_family   = AF_INET;      // IPv4, matching the server's udp::v4()
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
            lastError_ = "getaddrinfo failed for " + hostport;
            return;
        }
        fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd_ != BAD_SOCKET && ::connect(fd_, res->ai_addr, (int)res->ai_addrlen) != 0) {
            CloseSocket(fd_); fd_ = BAD_SOCKET;
        }
        freeaddrinfo(res);
        if (fd_ == BAD_SOCKET) { lastError_ = "udp socket/connect failed"; return; }

        // Non-blocking so poll() drains without stalling the frame. A socket
        // that stays blocking would freeze the game on the first empty recv, so
        // refuse to go on with one rather than hang.
        if (!SetNonBlocking(fd_)) {
            CloseSocket(fd_); fd_ = BAD_SOCKET;
            lastError_ = "udp socket could not be made non-blocking";
            return;
        }
        open_ = true;
    }

    void send(const std::string& s) override {
        if (fd_ != BAD_SOCKET) ::send(fd_, s.data(), (int)s.size(), 0);
    }
    bool isOpen() const override { return open_; }

    std::vector<std::string> poll() override {
        std::vector<std::string> out;
        if (fd_ == BAD_SOCKET) return out;
        char buf[65536];   // one datagram
        for (;;) {
            // int, not ssize_t: that is POSIX, and one datagram fits either way.
            // On Windows a connected UDP socket also reports the ICMP
            // port-unreachable of a server that is down as WSAECONNRESET here;
            // like EWOULDBLOCK it just ends this drain, and the hello retry
            // carries on exactly as on macOS.
            const int n = (int)::recv(fd_, buf, (int)sizeof(buf), 0);
            if (n <= 0) break; // EWOULDBLOCK (no more data) or a transient error
            // Chunked message (server splits anything over the safe datagram
            // size - in practice the LARGE-map welcome - to dodge IP
            // fragmentation, which some routers drop; see netbin.h). Reassemble
            // here so the rest of the client only ever sees whole messages.
            if (n >= (int)nb::CHUNK_HEADER && (uint8_t)buf[0] == nb::CHUNK_VERSION) {
                std::string whole;
                if (reasm_.Feed(buf, (size_t)n, whole)) out.push_back(std::move(whole));
                continue;
            }
            out.emplace_back(buf, buf + n);
        }
        return out;
    }

    std::string lastError() override { return lastError_; }

    void stop() override {
        if (fd_ != BAD_SOCKET) { CloseSocket(fd_); fd_ = BAD_SOCKET; }
        open_ = false;
    }

private:
    SocketHandle fd_  = BAD_SOCKET;
    bool        open_ = false;
    std::string lastError_;

    // Chunk reassembly. The logic lives in nb::ChunkReassembler (netbin.h) beside
    // the framing it implements, and because it needs testing without a socket.
    //
    // It used to be a single half-built message here, which lost a welcome
    // whenever a chunked state packet arrived mid-assembly (#100).
    nb::ChunkReassembler reasm_;
};

std::unique_ptr<ITransport> MakeTransport(const std::string& url) {
    if (url.rfind("udp://", 0) == 0) return std::make_unique<UdpTransport>();
    return std::make_unique<WsTransport>();
}

#endif // !__EMSCRIPTEN__
