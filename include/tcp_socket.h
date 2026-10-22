#pragma once

#include <ip_endpoint.h>

#include <cstddef>
#include <optional>
#include <string>
#include <sys/types.h>

// A TCP socket, owned: its file descriptor is closed when it goes. The system calls sc-tcp needs
// (listen, accept, a non-blocking connect, reads and writes), nothing more; addresses come from
// sc::ip_endpoint. tcp_server and tcp_client use it; so may anything else working with sockets.
namespace sc::tcp {
    class socket {
    public:
        socket() = default;
        explicit socket(int fd) : fd_(fd) {
        }
        ~socket() { close(); }
        socket(socket &&other) noexcept : fd_(other.release()) {
        }
        socket &operator=(socket &&other) noexcept;
        socket(const socket &) = delete;
        socket &operator=(const socket &) = delete;

        [[nodiscard]] int fd() const { return fd_; }
        [[nodiscard]] bool valid() const { return fd_ >= 0; }
        int release();
        void close();
        void shutdown();

        // A listening socket on the endpoint (the first of its addresses that binds), non-blocking.
        // Throws std::runtime_error naming the endpoint.
        static socket listen_on(const ip_endpoint &endpoint, int backlog);
        // A connection waiting to be accepted, made ready for use (non-blocking, no Nagle, keepalive,
        // no SIGPIPE); an invalid socket when there's none (errno says why: EAGAIN when none waits).
        static socket accept_from(const socket &listener, socket_address &peer);
        // Starts a non-blocking connect; in_progress says it's still going (wait until writable,
        // then connect_error()). An invalid socket when it failed at once (errno says why).
        static socket connect_to(const socket_address &address, bool &in_progress);
        // After a non-blocking connect: 0 when connected, else the error (errno's numbering).
        [[nodiscard]] int connect_error() const;

        // As much as it takes or has; -1 with errno set as recv/send set it.
        ssize_t read_some(char *data, std::size_t size) const;
        ssize_t write_some(const char *data, std::size_t size) const;

    private:
        void make_ready() const; // non-blocking, TCP_NODELAY, SO_KEEPALIVE, no SIGPIPE
        int fd_ = -1;
    };

    bool nonblocking(int fd);
}
