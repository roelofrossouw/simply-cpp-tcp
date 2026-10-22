#pragma once

#include "tcp_connection.h"
#include "tcp_handlers.h"

#include <ip_endpoint.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>

// A TCP client with the same handlers as sc::tcp_server: it connects to a server, and reconnects
// when the connection drops, if asked to.
//
//     sc::tcp_client client{{"listener.example.com", 5800}, {
//         .connected = [](sc::tcp_connection &c) { c.send(hello); },
//         .received = received,
//     }, {.reconnect = std::chrono::seconds{5}}};
//     std::thread talking{[&] { client.run(); }};
//     client.wait_until_connected();
//     client.send("...");
namespace sc {
    struct tcp_client_options {
        std::chrono::milliseconds connect_timeout{10000}; // per address tried
        // Wait this long, then connect again after a failed attempt or a dropped connection;
        // doubled each failure up to max_reconnect, back to it once connected. 0: don't (run()
        // returns once the connection is gone).
        std::chrono::milliseconds reconnect{0};
        std::chrono::milliseconds max_reconnect{60000};
        std::size_t threads = 1;                    // workers running the handlers
        std::chrono::milliseconds idle_timeout{0};  // close the connection when silent this long; 0: never
        std::chrono::milliseconds tick{0};          // tcp_handlers::tick this often; 0: never
        std::size_t max_input = 1 << 20;
        std::size_t max_output = 4 << 20;
    };

    class tcp_client {
    public:
        tcp_client(ip_endpoint server, tcp_handlers handlers, tcp_client_options options = {});
        ~tcp_client();

        tcp_client(const tcp_client &) = delete;
        tcp_client &operator=(const tcp_client &) = delete;

        // Connects and talks until stop(), or, without reconnect, until the connection is gone or
        // couldn't be made (std::runtime_error, saying why, for that). Usually on a thread of its
        // own.
        void run();
        // Closes the connection (disconnected with stopped) and makes run() return.
        void stop();
        // Waits until connected; false after timeout, or once run() has ended.
        bool wait_until_connected(std::chrono::milliseconds timeout = std::chrono::seconds{10});

        // The connection now; null while there is none.
        [[nodiscard]] std::shared_ptr<tcp_connection> connection() const;
        // Sends on the connection now; false when there is none (or it fails, as tcp_connection::send).
        bool send(std::string_view bytes);
        [[nodiscard]] tcp_stats stats() const;

        // The same as sc::tcp_log_level().
        static void log_level(sc::log_level level);

    private:
        class implementation;
        std::unique_ptr<implementation> implementation_;
    };
}
