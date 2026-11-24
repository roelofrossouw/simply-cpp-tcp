#pragma once

#include <sc/net/ip_endpoint.h>

#include <any>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

namespace sc::tcp {
    class event_loop;
    struct connection_state;

    // Why a connection ended, for tcp::handlers::disconnected.
    enum class close_reason {
        peer,            // the other side closed it
        closed,          // tcp::connection::close(), or the client was stopped
        idle,            // nothing received for the idle timeout
        error,           // a socket error, or a handler threw
        input_overflow,  // more unconsumed input than max_input
        output_overflow, // more unsent output than max_output
        stopped,         // tcp::server::stop() or tcp::client::stop()
        server_stopped = stopped, // its name before 2.0.0
    };

    // "peer", "closed", "idle" ...
    std::string_view close_reason_name(tcp::close_reason reason);

    // One TCP connection, a server's to a client or a client's to its server, as the handlers get
    // it. tcp::server::connections() and tcp::client::connection() hand it out as a shared_ptr, which
    // may be kept: once the connection is closed it fails cleanly. Everything here may be called
    // from any thread.
    class connection {
    public:
        ~connection();
        connection(const tcp::connection &) = delete;
        tcp::connection &operator=(const tcp::connection &) = delete;

        [[nodiscard]] std::uint64_t id() const;          // unique in its server's or client's life
        [[nodiscard]] const ip_endpoint &remote() const; // the other end's address and port
        [[nodiscard]] const ip_endpoint &local() const;  // this end's (a server's: which listening port)
        [[nodiscard]] std::chrono::system_clock::time_point connected_at() const;
        [[nodiscard]] bool open() const;

        // Sends the bytes, now or as soon as the socket takes them; false when the connection is
        // closed or closing, or when it would hold more than max_output (it's then closed).
        bool send(std::string_view bytes);
        // Closes it once what was sent is out (within 5 s); disconnected follows with
        // tcp::close_reason::closed.
        void close();

        // The handlers' own state for this connection (a parser, a device ID ...): only the
        // handlers touch it, so it needs no lock.
        [[nodiscard]] std::any &context();

    private:
        friend class event_loop;
        explicit connection(std::shared_ptr<tcp::connection_state> state);
        std::shared_ptr<tcp::connection_state> state_;
    };
}
