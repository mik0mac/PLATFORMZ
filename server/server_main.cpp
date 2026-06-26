// server/server_main.cpp
//
// PLATFORMZ test server - step 1: game headers compile on Linux.
//
// Adds over the previous version:
//   - GameSpace instantiated and generate() called on startup
//   - Fixed-timestep sim loop: updatePositions + RunCollisionChecks each tick
//   - Heartbeat now reports live asteroid/player counts from real game state
//
// raylib_server_stub.h (in this directory) shadows raylib.h/raymath.h/rlgl.h
// so game headers compile without a graphics library on the Linux VM.
//
// BUILD: make  (from server/ directory)
//
// TEST (browser console after connecting):
//   const ws = new WebSocket('wss://your-tunnel.trycloudflare.com');
//   ws.onmessage = e => console.log(e.data);
//   ws.onopen = () => ws.send('ping');
//   // expect: pong, then every second:
//   // tick:60 players:1 asteroids:8

// Game logic headers - raylib_server_stub.h is injected via -include in the
// Makefile, so raylib.h/raymath.h/rlgl.h includes in these headers are
// intercepted by the stub's header guards before they can fail.
#include "../gamespace.h"
#include "../collisions.h"

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
#include <chrono>
#include <vector>

namespace beast     = boost::beast;
namespace websocket = boost::beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;

const unsigned short PORT      = 9000;
const float          TICK_RATE = 60.0f;
const float          TICK_DT   = 1.0f / TICK_RATE;

// -------------------------------------------------------------------------
// Shared game state
// -------------------------------------------------------------------------
GameSpace     gameSpace;
CollisionGrid collisionGrid;
std::mutex    gameMutex;
std::atomic<uint32_t> serverTick{0};

// Active WebSocket sessions for broadcast
std::set<void*> activeSessions;
std::mutex      sessionMutex;

// -------------------------------------------------------------------------
// Session - one WebSocket connection
// -------------------------------------------------------------------------
class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket) : ws_(std::move(socket)) {}

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

    void Send(const std::string& msg) {
        beast::error_code ec;
        ws_.write(net::buffer(msg), ec);
    }

private:
    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;

    void Read() {
        ws_.async_read(buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
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
                    beast::error_code wec;
                    self->ws_.write(net::buffer(std::string("pong")), wec);
                }
                self->Read();
            });
    }
};

// -------------------------------------------------------------------------
// Broadcast to all sessions
// -------------------------------------------------------------------------
void Broadcast(const std::string& msg) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    for (void* ptr : activeSessions)
        static_cast<Session*>(ptr)->Send(msg);
}

// -------------------------------------------------------------------------
// Simulation loop - fixed timestep, broadcasts status every second
// -------------------------------------------------------------------------
void SimulationLoop() {
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::duration<double>;

    auto lastTick   = Clock::now();
    auto lastReport = Clock::now();

    while (true) {
        auto now     = Clock::now();
        auto elapsed = Duration(now - lastTick).count();

        if (elapsed < TICK_DT) {
            std::this_thread::sleep_for(std::chrono::microseconds(
                (int)((TICK_DT - elapsed) * 900000)));
            continue;
        }
        lastTick = now;

        {
            std::lock_guard<std::mutex> lock(gameMutex);
            gameSpace.updatePositions(TICK_DT);
            RunCollisionChecks(gameSpace, collisionGrid);
            gameSpace.updateActiveObjects();
            serverTick++;
        }

        // Broadcast status once per second
        auto reportElapsed = Duration(now - lastReport).count();
        if (reportElapsed >= 1.0) {
            lastReport = now;
            std::lock_guard<std::mutex> lock(gameMutex);
            std::string status =
                "tick:"      + std::to_string(serverTick.load()) +
                " players:"  + std::to_string(gameSpace.getPlayers().size()) +
                " asteroids:"+ std::to_string(gameSpace.getAsteroids().size());
            std::cout << status << "\n";
            Broadcast(status);
        }
    }
}

// -------------------------------------------------------------------------
// Listener - accepts TCP connections, spawns a Session per connection
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
    std::cout << "PLATFORMZ server | port " << PORT << "\n";

    {
        std::lock_guard<std::mutex> lock(gameMutex);
        gameSpace.generate();
        std::cout << "GameSpace generated: "
                  << gameSpace.getAsteroids().size() << " asteroids, "
                  << gameSpace.getPlatforms().size() << " platforms, "
                  << gameSpace.getPlayers().size()   << " players\n";
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
