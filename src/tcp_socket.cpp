#include "tcp_socket.h"

#include <fcntl.h>
#include <netinet/in.h>  // IPPROTO_IPV6, IPV6_V6ONLY
#include <netinet/tcp.h> // TCP_NODELAY
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace sc::tcp {
    bool nonblocking(const int fd) {
        const int flags = fcntl(fd, F_GETFL, 0);
        return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    socket &socket::operator=(socket &&other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.release();
        }
        return *this;
    }

    int socket::release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void socket::close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    void socket::shutdown() {
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
    }

    socket socket::listen_on(const ip_endpoint &endpoint, const int backlog) {
        std::vector<socket_address> addresses;
        try {
            addresses = endpoint.resolve(ip_use::listen);
        } catch (const std::runtime_error &error) {
            throw std::runtime_error(std::string{"sc-tcp: "} + error.what());
        }
        std::string failure = "no address";
        for (const auto &address: addresses) {
            socket candidate{::socket(address.family(), address.socket_type(), address.protocol())};
            if (!candidate.valid()) {
                failure = std::strerror(errno);
                continue;
            }
            const int yes = 1;
            setsockopt(candidate.fd(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
            if (address.ipv6()) setsockopt(candidate.fd(), IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof yes);
            if (bind(candidate.fd(), address.get(), address.size()) != 0 || ::listen(candidate.fd(), backlog) != 0 ||
                !nonblocking(candidate.fd())) {
                failure = std::strerror(errno);
                continue;
            }
            return candidate;
        }
        throw std::runtime_error("sc-tcp: can't listen on " + endpoint.to_string() + ": " + failure);
    }

    socket socket::accept_from(const socket &listener, socket_address &peer) {
        socket accepted{::accept(listener.fd(), peer.get(), peer.size_pointer())};
        if (accepted.valid()) accepted.make_ready();
        return accepted;
    }

    socket socket::connect_to(const socket_address &address, bool &in_progress) {
        in_progress = false;
        socket s{::socket(address.family(), address.socket_type(), address.protocol())};
        if (!s.valid()) return s;
        s.make_ready();
        if (::connect(s.fd(), address.get(), address.size()) == 0) return s;
        if (errno == EINPROGRESS) {
            in_progress = true;
            return s;
        }
        const int error = errno;
        s.close();
        errno = error;
        return s;
    }

    int socket::connect_error() const {
        int error = 0;
        socklen_t size = sizeof error;
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &size) != 0) return errno;
        return error;
    }

    ssize_t socket::read_some(char *data, const std::size_t size) const {
        return ::recv(fd_, data, size, 0);
    }

    // Without SIGPIPE: MSG_NOSIGNAL on Linux, SO_NOSIGPIPE (make_ready) on macOS.
    ssize_t socket::write_some(const char *data, const std::size_t size) const {
#ifdef MSG_NOSIGNAL
        return ::send(fd_, data, size, MSG_NOSIGNAL);
#else
        return ::send(fd_, data, size, 0);
#endif
    }

    void socket::make_ready() const {
        nonblocking(fd_);
        const int yes = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof yes);
#ifdef SO_NOSIGPIPE
        setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
    }
}
