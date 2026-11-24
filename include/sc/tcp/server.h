#pragma once

#include <sc/tcp/connection.h>
#include <sc/tcp/handlers.h>

#include <sc/net/ip_endpoint.h>
#include <sc/net/ip_endpoints.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

// A TCP server for many long-lived connections: devices that connect and keep talking, such as
// GPS trackers. One thread waits on every socket (epoll on Linux, kqueue on macOS); a few worker
// threads run the handlers.
//
//     sc::tcp::server server{"0.0.0.0:5400;0.0.0.0:5500", {
//         .connected = connected,
//         .received = received,      // std::size_t received(sc::tcp::connection &, std::string_view input)
//     }};
//     std::thread serving{[&] { server.run(); }};
namespace sc::tcp {
    struct server_options {
        std::size_t threads = 0;                     // workers running handlers; 0: one a CPU, at least 2
        std::chrono::milliseconds idle_timeout{0};   // close a connection silent this long; 0: never
        std::chrono::milliseconds tick{0};           // tcp::handlers::tick for each connection this often; 0: never
        std::size_t max_connections = 10000;         // more are accepted and closed at once
        std::size_t max_input = 1 << 20;             // unconsumed input a connection may hold
        std::size_t max_output = 4 << 20;            // unsent output a connection may hold
        int backlog = 512;                           // connections waiting to be accepted
    };

    using server_stats = tcp::stats;

    class server {
    public:
        // listen: one or more host:port, ';'-separated ("0.0.0.0:5400;[::]:5400"); an ip_endpoint
        // with port 0 ({"127.0.0.1", 0}) gets a free one (see listening()). Binds when run() starts.
        server(ip_endpoints listen, tcp::handlers handlers, tcp::server_options options = {});
        ~server();

        server(const tcp::server &) = delete;
        tcp::server &operator=(const tcp::server &) = delete;

        // Serves until stop(): usually on a thread of its own. Throws std::runtime_error when an
        // endpoint can't be bound, or when it is already running.
        void run();
        // Closes every connection (disconnected with stopped), lets the handlers finish
        // and makes run() return. From any thread, a handler's included.
        void stop();
        // Waits until run() is accepting; false when it failed first, or after timeout.
        bool wait_until_running(std::chrono::milliseconds timeout = std::chrono::seconds{10});

        // Where it listens, with the ports bound (after wait_until_running()).
        [[nodiscard]] std::vector<ip_endpoint> listening() const;
        [[nodiscard]] tcp::stats stats() const;
        // The open connections, say to send to one by an ID the handlers keep in its context().
        [[nodiscard]] std::vector<std::shared_ptr<tcp::connection>> connections() const;

        // The same as sc::tcp::log_level().
        static void log_level(sc::log_level level);

    private:
        class implementation;
        std::unique_ptr<implementation> implementation_;
    };
}
