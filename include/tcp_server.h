#pragma once

#include <ip_endpoint.h>
#include <ip_endpoints.h>
#include <log_level.h>

#include <any>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// A TCP server for many long-lived connections: devices that connect and keep talking, such as
// GPS trackers. One thread waits on every socket (epoll on Linux, kqueue on macOS); a few worker
// threads run the handlers. A connection's handlers run one at a time, in order, and never two at
// once; different connections' run side by side.
//
//     sc::tcp_server server{"0.0.0.0:5400;0.0.0.0:5500", {
//         .connected = [](sc::tcp_connection &c) { ... },
//         .received = [](sc::tcp_connection &c, std::string_view input) -> std::size_t {
//             if (input.size() < 4) return 0;   // wait for more
//             c.send("\x01");
//             return 4;                          // consumed; the rest stays for next time
//         },
//     }};
//     std::thread serving{[&] { server.run(); }};
namespace sc {
    // Why a connection ended, for tcp_handlers::disconnected.
    enum class tcp_close_reason {
        peer,            // the other side closed it
        closed,          // tcp_connection::close()
        idle,            // nothing received for options.idle_timeout
        error,           // a socket error, or a handler threw
        input_overflow,  // more unconsumed input than options.max_input
        output_overflow, // more unsent output than options.max_output
        server_stopped,  // tcp_server::stop()
    };

    // "peer", "closed", "idle" ...
    std::string_view tcp_close_reason_name(tcp_close_reason reason);

    struct tcp_server_options {
        std::size_t threads = 0;                     // workers running handlers; 0: one a CPU, at least 2
        std::chrono::milliseconds idle_timeout{0};   // close a connection silent this long; 0: never
        std::chrono::milliseconds tick{0};           // tcp_handlers::tick for each connection this often; 0: never
        std::size_t max_connections = 10000;         // more are accepted and closed at once
        std::size_t max_input = 1 << 20;             // unconsumed input a connection may hold
        std::size_t max_output = 4 << 20;            // unsent output a connection may hold
        int backlog = 512;                           // connections waiting to be accepted
    };

    // One connection, as the handlers get it (and may keep, as the shared_ptr from
    // tcp_server::connections()). Everything here may be called from any thread.
    class tcp_connection {
    public:
        virtual ~tcp_connection() = default;

        [[nodiscard]] virtual std::uint64_t id() const = 0;        // unique in the server's life
        [[nodiscard]] virtual const ip_endpoint &remote() const = 0; // the client's address and port
        [[nodiscard]] virtual const ip_endpoint &local() const = 0;  // the server's (which listening port)
        [[nodiscard]] virtual std::chrono::system_clock::time_point connected_at() const = 0;
        [[nodiscard]] virtual bool open() const = 0;

        // Sends the bytes, now or as soon as the socket takes them; false when the connection is
        // closed or closing, or when it would hold more than max_output (it's then closed).
        virtual bool send(std::string_view bytes) = 0;
        // Closes it once what was sent is out; disconnected follows with tcp_close_reason::closed.
        virtual void close() = 0;

        // The handlers' own state for this connection (a parser, an IMEI ...): only the handlers
        // touch it, so it needs no lock.
        [[nodiscard]] virtual std::any &context() = 0;
    };

    struct tcp_handlers {
        // First, once accepted.
        std::function<void(tcp_connection &)> connected;
        // When bytes arrive: input is everything received and not yet consumed. Returns how many
        // bytes from the start it consumed; the rest stays, and more is added to it. Returning 0
        // waits for more.
        std::function<std::size_t(tcp_connection &, std::string_view input)> received;
        // Every options.tick, between the others.
        std::function<void(tcp_connection &)> tick;
        // Last, once it's closed: nothing more can be sent.
        std::function<void(tcp_connection &, tcp_close_reason)> disconnected;
    };

    struct tcp_server_stats {
        std::size_t connections = 0;   // open now
        std::uint64_t accepted = 0;    // since the start
        std::uint64_t refused = 0;     // over max_connections
        std::uint64_t bytes_received = 0;
        std::uint64_t bytes_sent = 0;
    };

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
        // Closes every connection (disconnected with server_stopped), lets the handlers finish
        // and makes run() return. From any thread, a handler's included.
        void stop();
        // Waits until run() is accepting; false when it failed first, or after timeout.
        bool wait_until_running(std::chrono::milliseconds timeout = std::chrono::seconds{10});

        // Where it listens, with the ports bound (after wait_until_running()).
        [[nodiscard]] std::vector<ip_endpoint> listening() const;
        [[nodiscard]] tcp_server_stats stats() const;
        // The open connections, say to send to one by an ID the handlers keep in its context().
        [[nodiscard]] std::vector<std::shared_ptr<tcp_connection>> connections() const;

        // What the server writes to stderr (a handler that threw, a refused connection): warnings
        // and up by default, for every server in the process.
        static void log_level(sc::log_level level);

    private:
        class implementation;
        std::unique_ptr<implementation> implementation_;
    };
}
