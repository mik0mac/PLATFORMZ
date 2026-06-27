// net_client.h
//
// Client-side WebSocket wrapper around IXWebSocket. Transport-agnostic: the URL
// is the only thing that changes between LAN and internet -
//   ws://192.168.1.20:9000   (LAN, no TLS)
//   wss://xxx.trycloudflare.com  (internet, TLS via macOS Secure Transport)
// - the rest of the client code is identical.
//
// IXWebSocket runs its own background thread and delivers messages through a
// callback. We push inbound text frames onto a mutex-guarded queue; the game
// loop drains it once per frame with poll() (so all game state is still touched
// only from the main thread). Nothing here knows about the game - it's a dumb
// pipe of strings.

#pragma once

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

#include <string>
#include <vector>
#include <deque>
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
