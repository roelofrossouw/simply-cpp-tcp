#pragma once

#include <chrono>
#include <vector>

// Waiting on many sockets at once: epoll on Linux, kqueue on macOS, behind one small interface.
// Read interest always, write interest while output waits. Thread-safe, as the kernel calls are,
// so a send() on a handler's thread can ask for write interest.
namespace sc::tcp {
    class poller {
    public:
        poller();
        ~poller();
        poller(const poller &) = delete;
        poller &operator=(const poller &) = delete;

        void add(int fd);
        void want_write(int fd, bool wanted);
        void remove(int fd);

        struct event {
            int fd;
            bool readable, writable;
        };
        // Waits up to timeout; the events since.
        std::vector<event> wait(std::chrono::milliseconds timeout);

    private:
        int fd_ = -1;
    };
}
