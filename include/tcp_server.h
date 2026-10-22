#pragma once

#include "tcp_connection.h"
#include "tcp_handlers.h"

#include <ip_endpoint.h>
#include <ip_endpoints.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

// A TCP server for many long-lived connections: devices that connect and keep talking, such as
// GPS trackers. One thread waits on every socket (epoll on Linux, kqueue on macOS); a few worker
// threads run the handlers.
//
//     sc::tcp_server server{"0.0.0.0:5400;0.0.0.0:5500", {
//         .connected = connected,
//         .received = received,      // std::size_t received(sc::tcp_connection &, std::string_view input)
//     }};
//     std::thread serving{[&] { server.run(); }};
namespace sc {
    struct tcp_server_options {
        std::size_t threads = 0;                     // workers running handlers; 0: one a CPU, at least 2
        std::chrono::milliseconds idle_timeout{0};   // close a connection silent this long; 0: never
        std::chrono::milliseconds tick{0};           // tcp_handlers::tick for each connection this often; 0: never
        std::size_t max_connections = 10000;         // more are accepted and closed at once
        std::size_t max_input = 1 << 20;             // unconsumed input a connection may hold
        std::size_t max_output = 4 << 20;            // unsent output a connection may hold
        int backlog = 512;                           // connections waiting to be accepted
    };

    using tcp_server_stats = tcp_stats;

    class tcp_server {
    public:
        // listen: one or more host:port, ';'-separated ("0.0.0.0:5400;[::]:5400"); an ip_endpoint
        // with port 0 ({"127.0.0.1", 0}) gets a free one (see listening()). Binds when run() starts.
        tcp_server(ip_endpoints listen, tcp_handlers handlers, tcp_server_options options = {});
        ~tcp_server();

        tcp_server(const tcp_server &) = delete;
        tcp_server &operator=(const tcp_server &) = delete;

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
        [[nodiscard]] tcp_stats stats() const;
        // The open connections, say to send to one by an ID the handlers keep in its context().
        [[nodiscard]] std::vector<std::shared_ptr<tcp_connection>> connections() const;

        // The same as sc::tcp_log_level().
        static void log_level(sc::log_level level);

    private:
        class implementation;
        std::unique_ptr<implementation> implementation_;
    };
}
