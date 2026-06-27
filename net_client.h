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
// Desktop backend: IXWebSocket (vendored). Runs its own background thread and
// delivers messages through a callback; we push inbound text frames onto a
// mutex-guarded queue and drain it once per frame so game state is still only
// touched from the main thread.
// ---------------------------------------------------------------------------
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

#include <mutex>
#include <atomic>

class NetClient {
public:
    NetClient()  { ix::initNetSystem(); }
    ~NetClient() { ws_.stop(); ix::uninitNetSystem(); }

    // Begin connecting (non-blocking). IXWebSocket retries with backoff on its
    // own thread, so a server that isn't up yet - or a dropped connection -
    // recovers without any extra code here.
    void connect(const std::string& url) {
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

    void send(const std::string& s) { ws_.send(s); }
    bool isOpen() const { return open_.load(); }

    // Drain every queued inbound frame (oldest first). Called once per frame.
    std::vector<std::string> poll() {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lk(mtx_);
        out.reserve(inbox_.size());
        while (!inbox_.empty()) { out.push_back(std::move(inbox_.front())); inbox_.pop_front(); }
        return out;
    }

    std::string lastError() {
        std::lock_guard<std::mutex> lk(mtx_);
        return lastError_;
    }

    void stop() { ws_.stop(); }

private:
    ix::WebSocket           ws_;
    std::mutex              mtx_;
    std::deque<std::string> inbox_;     // guarded by mtx_
    std::string             lastError_; // guarded by mtx_
    std::atomic<bool>       open_{false};
};

#endif // __EMSCRIPTEN__
