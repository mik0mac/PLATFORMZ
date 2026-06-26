// server/server_main.cpp
//
// Minimal PLATFORMZ connectivity test server.
// Goal: confirm the full pipeline works (GitHub Actions VM -> build ->
// Cloudflare tunnel -> browser WebSocket) before any game logic is added.
//
// What this does:
//   - Listens for WebSocket connections on port 9000
//   - Responds to a "ping" message with "pong"
//   - Broadcasts a "heartbeat:<tick>" message to all clients every second
//
// Uses Boost.Beast (WebSockets) + Boost.Asio (async I/O).
// Boost is pre-installed on ubuntu-latest GitHub Actions runners.
//
// BUILD (Linux / GitHub Actions):
//   g++ server_main.cpp -o gameserver -std=c++17 -lboost_system -lpthread
//
// BUILD (mac, local test):
//   brew install boost
//   g++ server_main.cpp -o gameserver -std=c++17 \
//     -I/opt/homebrew/include -L/opt/homebrew/lib -lboost_system -lpthread
//
// TEST (browser dev console):
//   const ws = new WebSocket('ws://localhost:9000');
//   ws.onmessage = e => console.log('received:', e.data);
//   ws.onopen = () => ws.send('ping');
//   // expect: received: pong
//   // then every second: received: heartbeat:1, heartbeat:2, ...

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <iostream>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>

namespace beast     = boost::beast;
namespace websocket = boost::beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;

const unsigned short PORT = 9000;

// All active sessions for broadcast. Protected by mutex.
std::set<void*> activeSessions;
std::mutex sessionMutex;
std::atomic<uint32_t> heartbeatTick{0};

// -------------------------------------------------------------------------
// Session - owns one WebSocket connection.
// -------------------------------------------------------------------------
class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket)
        : ws_(std::move(socket)) {}

    void Start() {
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (ec) { std::cerr << "accept: " << ec.message() << "\n"; return; }
            {
                std::lock_guard<std::mutex> lock(sessionMutex);
                activeSessions.insert(self.get());
                std::cout << "Client connected. Sessions: "
                          << activeSessions.size() << "\n";
            }
            self->Read();
        });
    }

    void Send(const std::string& message) {
        beast::error_code ec;
        ws_.write(net::buffer(message), ec);
        // Errors here mean the client disconnected; Read() will clean up.
    }

private:
    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;

    void Read() {
        ws_.async_read(buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    // Disconnected - remove from active set.
                    std::lock_guard<std::mutex> lock(sessionMutex);
                    activeSessions.erase(self.get());
                    std::cout << "Client disconnected. Sessions: "
                              << activeSessions.size() << "\n";
                    return;
                }

                std::string msg = beast::buffers_to_string(self->buffer_.data());
                self->buffer_.consume(self->buffer_.size());
                std::cout << "Received: " << msg << "\n";

                if (msg == "ping") {
                    beast::error_code writeEc;
                    self->ws_.write(net::buffer(std::string("pong")), writeEc);
                }

                self->Read(); // keep reading
            });
    }
};

// -------------------------------------------------------------------------
// Heartbeat - broadcasts to all sessions every second.
// Simulates the server's future game-tick broadcast.
// -------------------------------------------------------------------------
void HeartbeatLoop() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint32_t tick = ++heartbeatTick;
        std::string msg = "heartbeat:" + std::to_string(tick);

        std::lock_guard<std::mutex> lock(sessionMutex);
        std::cout << "Broadcasting " << msg << " to "
                  << activeSessions.size() << " clients\n";
        for (void* ptr : activeSessions) {
            static_cast<Session*>(ptr)->Send(msg);
        }
    }
}

// -------------------------------------------------------------------------
// Listener - accepts TCP connections and spawns a Session per connection.
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
    std::cout << "PLATFORMZ test server | port " << PORT << "\n";
    std::cout << "Send 'ping' -> receive 'pong'. Heartbeat every 1s.\n";

    const int threads = std::max(1u, std::thread::hardware_concurrency());
    net::io_context ioc{threads};

    auto listener = std::make_shared<Listener>(
        ioc, tcp::endpoint{net::ip::make_address("0.0.0.0"), PORT});
    listener->Run();

    std::thread(HeartbeatLoop).detach();

    std::vector<std::thread> pool;
    for (int i = 0; i < threads - 1; i++)
        pool.emplace_back([&ioc] { ioc.run(); });
    ioc.run();

    return 0;
}
