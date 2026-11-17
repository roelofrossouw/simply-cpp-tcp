#include "tcp_poller.h"

#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#include <sys/time.h>
#endif

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace sc::tcp {
    poller::poller() {
#ifdef __linux__
        fd_ = epoll_create1(EPOLL_CLOEXEC);
#else
        fd_ = kqueue();
#endif
        if (fd_ < 0) throw std::runtime_error(std::string{"sc-tcp: no poller: "} + std::strerror(errno));
    }

    poller::~poller() {
        ::close(fd_);
    }

    void poller::add(const int fd) {
#ifdef __linux__
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = fd;
        epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &event);
#else
        struct kevent change{};
        EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        kevent(fd_, &change, 1, nullptr, 0, nullptr);
#endif
    }

    void poller::want_write(const int fd, const bool wanted) {
#ifdef __linux__
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP | (wanted ? EPOLLOUT : 0u);
        event.data.fd = fd;
        epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &event);
#else
        struct kevent change{};
        EV_SET(&change, fd, EVFILT_WRITE, wanted ? EV_ADD | EV_ENABLE : EV_DELETE, 0, 0, nullptr);
        kevent(fd_, &change, 1, nullptr, 0, nullptr);
#endif
    }

    void poller::remove(const int fd) {
#ifdef __linux__
        epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr);
#else
        struct kevent changes[2];
        EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(fd_, changes, 2, nullptr, 0, nullptr); // the write one may not be there: fine
#endif
    }

    std::vector<poller::event> poller::wait(const std::chrono::milliseconds timeout) {
        std::vector<event> out;
#ifdef __linux__
        epoll_event events[256];
        const int n = epoll_wait(fd_, events, 256, static_cast<int>(timeout.count()));
        for (int i = 0; i < n; ++i) {
            const auto flags = events[i].events;
            out.push_back({events[i].data.fd, (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0,
                           (flags & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0});
        }
#else
        struct kevent events[256];
        timespec wait_for{static_cast<time_t>(timeout.count() / 1000), static_cast<long>(timeout.count() % 1000) * 1000000};
        const int n = kevent(fd_, nullptr, 0, events, 256, &wait_for);
        for (int i = 0; i < n; ++i) {
            out.push_back({static_cast<int>(events[i].ident), events[i].filter == EVFILT_READ, events[i].filter == EVFILT_WRITE});
        }
#endif
        return out;
    }
}
