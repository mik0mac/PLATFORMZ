// net_client.h
//
// Client-side WebSocket wrapper. Transport-agnostic: the URL is the only thing
// that changes between LAN and internet -
//   ws://192.168.1.20:9000      (LAN, no TLS)
//   wss://xxx.trycloudflare.com  (internet, TLS)
// - the rest of the client code is identical.
//
// Two backends behind one identical public API (connect / send / isOpen /
// poll / lastError / stop), so main.cpp never knows which is in use:
//   - Desktop: IXWebSocket (raw TCP + a background thread; mutex-guarded inbox).
//   - Browser (__EMSCRIPTEN__): the browser's own WebSocket via
//     emscripten/websocket.h. Raw TCP and threads don't exist in the WASM
//     sandbox, so all socket traffic must go through the JS WebSocket API. It's
//     callback-driven on the main thread, so no mutex is needed - the same
//     inbox + poll() drain model still applies.
//
// Nothing here knows about the game - it's a dumb pipe of strings.

#pragma once

#include <string>
#include <vector>
#include <deque>

#if defined(__EMSCRIPTEN__)
// ---------------------------------------------------------------------------
// Browser backend: emscripten WebSocket (link with -lwebsocket.js).
// ---------------------------------------------------------------------------
#include <emscripten/websocket.h>

class NetClient {
public:
    NetClient() = default;
    ~NetClient() { stop(); }

    // Begin connecting. The browser opens the socket asynchronously; onopen
    // flips open_ once the handshake completes. No retry/backoff here (the
    // desktop backend gets that from IXWebSocket); reconnect can be added if a
    // browser session needs to survive a server bounce.
    void connect(const std::string& url) {
        if (!emscripten_websocket_is_supported()) {
            lastError_ = "WebSocket not supported by this browser";
            return;
        }
        EmscriptenWebSocketCreateAttributes attr;
        emscripten_websocket_init_create_attributes(&attr);
        attr.url = url.c_str();
        attr.protocols = nullptr;
        ws_ = emscripten_websocket_new(&attr);
        if (ws_ <= 0) { lastError_ = "emscripten_websocket_new failed"; return; }

        emscripten_websocket_set_onopen_callback   (ws_, this, &NetClient::OnOpen);
        emscripten_websocket_set_onmessage_callback(ws_, this, &NetClient::OnMessage);
        emscripten_websocket_set_onerror_callback  (ws_, this, &NetClient::OnError);
        emscripten_websocket_set_onclose_callback  (ws_, this, &NetClient::OnClose);
    }

    void send(const std::string& s) {
        if (ws_ > 0) emscripten_websocket_send_utf8_text(ws_, s.c_str());
    }
    bool isOpen() const { return open_; }

    // Drain every queued inbound frame (oldest first). Called once per frame.
    std::vector<std::string> poll() {
        std::vector<std::string> out;
        out.reserve(inbox_.size());
        while (!inbox_.empty()) { out.push_back(std::move(inbox_.front())); inbox_.pop_front(); }
        return out;
    }

    std::string lastError() { return lastError_; }

    void stop() {
        if (ws_ > 0) {
            emscripten_websocket_close(ws_, 1000, "client closing");
            emscripten_websocket_delete(ws_);
            ws_ = 0;
        }
        open_ = false;
    }

private:
    // Static C trampolines: emscripten hands us back the `this` we registered.
    static EM_BOOL OnOpen(int, const EmscriptenWebSocketOpenEvent*, void* ud) {
        static_cast<NetClient*>(ud)->open_ = true;
        return EM_TRUE;
    }
    static EM_BOOL OnMessage(int, const EmscriptenWebSocketMessageEvent* e, void* ud) {
        // We only speak text (JSON). isText guards against any stray binary frame.
        if (e->isText && e->data)
            static_cast<NetClient*>(ud)->inbox_.emplace_back(
                reinterpret_cast<const char*>(e->data)); // text is null-terminated
        return EM_TRUE;
    }
    static EM_BOOL OnError(int, const EmscriptenWebSocketErrorEvent*, void* ud) {
        NetClient* self = static_cast<NetClient*>(ud);
        self->open_ = false;
        self->lastError_ = "websocket error";
        return EM_TRUE;
    }
    static EM_BOOL OnClose(int, const EmscriptenWebSocketCloseEvent*, void* ud) {
        static_cast<NetClient*>(ud)->open_ = false;
        return EM_TRUE;
    }

    EMSCRIPTEN_WEBSOCKET_T  ws_ = 0;
    std::deque<std::string> inbox_;     // main-thread only - no mutex needed
    std::string             lastError_;
    bool                    open_ = false;
};

#else
// ---------------------------------------------------------------------------
// Desktop backend: two transports behind one interface, picked by URL scheme so
// the public NetClient API (and main.cpp) never knows which is in use:
//   ws:// or wss://  -> WsTransport (IXWebSocket; also how the browser connects)
//   udp://           -> UdpTransport (raw UDP datagrams; native only)
// A native binary can therefore join either a WebSocket server or a UDP server;
// the server speaks both at once (see server/server_main.cpp).
// ---------------------------------------------------------------------------
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

#include "netbin.h"   // chunk framing constants (UDP reassembly in UdpTransport)

#include <mutex>
#include <atomic>
#include <memory>

// POSIX sockets for the UDP backend (macOS/Linux). No extra link flags.
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

// Common interface: connect / send / isOpen / poll / lastError / stop.
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void connect(const std::string& url) = 0;
    virtual void send(const std::string& s) = 0;
    virtual bool isOpen() const = 0;
    virtual std::vector<std::string> poll() = 0;
    virtual std::string lastError() = 0;
    virtual void stop() = 0;
};

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
    ~UdpTransport() override { stop(); }

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
        if (fd_ >= 0 && ::connect(fd_, res->ai_addr, res->ai_addrlen) < 0) {
            ::close(fd_); fd_ = -1;
        }
        freeaddrinfo(res);
        if (fd_ < 0) { lastError_ = "udp socket/connect failed"; return; }

        // Non-blocking so poll() drains without stalling the frame.
        const int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        open_ = true;
    }

    void send(const std::string& s) override {
        if (fd_ >= 0) ::send(fd_, s.data(), s.size(), 0);
    }
    bool isOpen() const override { return open_; }

    std::vector<std::string> poll() override {
        std::vector<std::string> out;
        if (fd_ < 0) return out;
        char buf[65536];   // one datagram
        for (;;) {
            const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break; // EWOULDBLOCK (no more data) or a transient error
            // Chunked message (server splits anything over the safe datagram
            // size - in practice the LARGE-map welcome - to dodge IP
            // fragmentation, which some routers drop; see netbin.h). Reassemble
            // here so the rest of the client only ever sees whole messages.
            if (n >= (ssize_t)nb::CHUNK_HEADER && (uint8_t)buf[0] == nb::CHUNK_VERSION) {
                Reassemble(buf, (size_t)n, out);
                continue;
            }
            out.emplace_back(buf, buf + n);
        }
        return out;
    }

    std::string lastError() override { return lastError_; }

    void stop() override {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        open_ = false;
    }

private:
    int         fd_   = -1;
    bool        open_ = false;
    std::string lastError_;

    // Chunk reassembly - one logical message at a time, keyed by the server's
    // gen byte. A chunk from a different gen (or a different chunk count)
    // discards the half-done buffer: chunks of one message never interleave
    // with another's, and a lost chunk is recovered by the hello-retry loop
    // requesting a fresh (new-gen) welcome, not by any per-chunk ack.
    std::vector<std::string> parts_;
    uint8_t chunkGen_  = 0;
    size_t  partsHave_ = 0;

    void Reassemble(const char* buf, size_t n, std::vector<std::string>& out) {
        uint8_t gen   = (uint8_t)buf[1];
        uint8_t index = (uint8_t)buf[2];
        uint8_t count = (uint8_t)buf[3];
        if (count == 0 || index >= count) return; // malformed
        if (gen != chunkGen_ || parts_.size() != (size_t)count) {
            parts_.assign(count, {});
            partsHave_ = 0;
            chunkGen_  = gen;
        }
        if (!parts_[index].empty()) return; // duplicate datagram
        parts_[index].assign(buf + nb::CHUNK_HEADER, buf + n);
        if (++partsHave_ < count) return;
        std::string whole;
        for (const std::string& p : parts_) whole += p;
        out.push_back(std::move(whole));
        parts_.clear();
        partsHave_ = 0;
    }
};

// --- Public client: picks the transport by URL scheme -----------------------
class NetClient {
public:
    NetClient() = default;
    ~NetClient() { if (impl_) impl_->stop(); }

    void connect(const std::string& url) {
        if (url.rfind("udp://", 0) == 0) impl_ = std::make_unique<UdpTransport>();
        else                             impl_ = std::make_unique<WsTransport>();
        impl_->connect(url);
    }

    void send(const std::string& s)          { if (impl_) impl_->send(s); }
    bool isOpen() const                      { return impl_ && impl_->isOpen(); }
    std::vector<std::string> poll()          { return impl_ ? impl_->poll() : std::vector<std::string>{}; }
    std::string lastError()                  { return impl_ ? impl_->lastError() : std::string(); }
    void stop()                              { if (impl_) impl_->stop(); }

private:
    std::unique_ptr<ITransport> impl_;
};

#endif // __EMSCRIPTEN__
