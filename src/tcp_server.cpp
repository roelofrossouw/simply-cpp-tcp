#include "tcp_server.h"

#include <fcntl.h>
#include <netinet/in.h>  // IPPROTO_IPV6, IPV6_V6ONLY: socket options; addresses are sc::ip_endpoint's
#include <netinet/tcp.h> // TCP_NODELAY
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {
    std::atomic<sc::log_level> log_level{sc::log_level::warning};

    void warn(const std::string &text) {
        if (log_level.load() >= sc::log_level::warning) std::cerr << "sc-tcp-server: " << text << std::endl;
    }

    using steady = std::chrono::steady_clock;

    bool nonblocking(const int fd) {
        const int flags = fcntl(fd, F_GETFL, 0);
        return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    // Writes without SIGPIPE: MSG_NOSIGNAL on Linux, SO_NOSIGPIPE (set on accept) on macOS.
    ssize_t write_some(const int fd, const char *data, const std::size_t size) {
#ifdef MSG_NOSIGNAL
        return ::send(fd, data, size, MSG_NOSIGNAL);
#else
        return ::send(fd, data, size, 0);
#endif
    }

    // epoll or kqueue: read interest always, write interest while output waits. Thread-safe, as the
    // kernel calls are: send() from a handler's thread asks for write interest directly.
    class poller {
    public:
        poller() {
#ifdef __linux__
            fd_ = epoll_create1(EPOLL_CLOEXEC);
#else
            fd_ = kqueue();
#endif
            if (fd_ < 0) throw std::runtime_error(std::string{"sc-tcp-server: no poller: "} + std::strerror(errno));
        }
        ~poller() { ::close(fd_); }
        poller(const poller &) = delete;
        poller &operator=(const poller &) = delete;

        void add(const int fd) {
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

        void want_write(const int fd, const bool wanted) {
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

        void remove(const int fd) {
#ifdef __linux__
            epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr);
#else
            struct kevent changes[2];
            EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
            kevent(fd_, changes, 2, nullptr, 0, nullptr); // the write one may not be there: fine
#endif
        }

        struct event {
            int fd;
            bool readable, writable, hung_up;
        };

        // Waits up to timeout; the events since.
        std::vector<event> wait(const std::chrono::milliseconds timeout) {
            std::vector<event> out;
#ifdef __linux__
            epoll_event events[256];
            const int n = epoll_wait(fd_, events, 256, static_cast<int>(timeout.count()));
            for (int i = 0; i < n; ++i) {
                const auto flags = events[i].events;
                out.push_back({events[i].data.fd, (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0, (flags & EPOLLOUT) != 0,
                               (flags & (EPOLLHUP | EPOLLERR)) != 0});
            }
#else
            struct kevent events[256];
            timespec wait_for{static_cast<time_t>(timeout.count() / 1000), static_cast<long>(timeout.count() % 1000) * 1000000};
            const int n = kevent(fd_, nullptr, 0, events, 256, &wait_for);
            for (int i = 0; i < n; ++i) {
                const int fd = static_cast<int>(events[i].ident);
                const bool read = events[i].filter == EVFILT_READ;
                out.push_back({fd, read, events[i].filter == EVFILT_WRITE, read && (events[i].flags & EV_EOF) && events[i].data == 0});
            }
#endif
            return out;
        }

    private:
        int fd_ = -1;
    };
}

namespace sc {
    std::string_view tcp_close_reason_name(const tcp_close_reason reason) {
        switch (reason) {
            case tcp_close_reason::peer: return "peer";
            case tcp_close_reason::closed: return "closed";
            case tcp_close_reason::idle: return "idle";
            case tcp_close_reason::error: return "error";
            case tcp_close_reason::input_overflow: return "input_overflow";
            case tcp_close_reason::output_overflow: return "output_overflow";
            case tcp_close_reason::server_stopped: return "server_stopped";
        }
        return "unknown";
    }

    void tcp_server::log_level(const sc::log_level level) {
        ::log_level = level;
    }
}

namespace {
    class worker_pool;

    // One connection. The I/O thread reads into `arrived`; its handlers run as tasks, one at a
    // time (the strand: a queue and whether it's in the pool's queue), on the workers.
    class connection final : public sc::tcp_connection, public std::enable_shared_from_this<connection> {
    public:
        connection(const std::uint64_t id, const int fd, sc::ip_endpoint remote, sc::ip_endpoint local, poller &events,
                   const sc::tcp_server_options &options, std::function<void(std::uint64_t)> wake_to_close)
            : id_(id), fd_(fd), remote_(std::move(remote)), local_(std::move(local)), events_(events), options_(options),
              wake_to_close_(std::move(wake_to_close)), connected_at_(std::chrono::system_clock::now()),
              last_received_(steady::now()), next_tick_(steady::now() + options.tick) {
        }

        ~connection() override {
            if (fd_ >= 0) ::close(fd_);
        }

        [[nodiscard]] std::uint64_t id() const override { return id_; }
        [[nodiscard]] const sc::ip_endpoint &remote() const override { return remote_; }
        [[nodiscard]] const sc::ip_endpoint &local() const override { return local_; }
        [[nodiscard]] std::chrono::system_clock::time_point connected_at() const override { return connected_at_; }
        [[nodiscard]] bool open() const override { return !closing_.load(); }
        [[nodiscard]] std::any &context() override { return context_; }

        bool send(const std::string_view bytes) override {
            const std::lock_guard lock{output_mutex_};
            if (closing_ || fd_ < 0) return false;
            std::size_t offset = 0;
            if (output_.empty()) { // straight out, as far as the socket takes it
                while (offset < bytes.size()) {
                    const auto n = write_some(fd_, bytes.data() + offset, bytes.size() - offset);
                    if (n > 0) {
                        offset += static_cast<std::size_t>(n);
                        sent_ += static_cast<std::uint64_t>(n);
                    } else if (n < 0 && errno == EINTR) {
                        continue;
                    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    } else {
                        request_close(sc::tcp_close_reason::error);
                        return false;
                    }
                }
            }
            if (offset < bytes.size()) {
                if (output_.size() + bytes.size() - offset > options_.max_output) {
                    request_close(sc::tcp_close_reason::output_overflow);
                    return false;
                }
                const bool was_empty = output_.empty();
                output_.append(bytes.substr(offset));
                if (was_empty) events_.want_write(fd_, true);
            }
            return true;
        }

        void close() override {
            const std::lock_guard lock{output_mutex_};
            request_close(sc::tcp_close_reason::closed);
        }

        // --- the I/O thread's side ---

        [[nodiscard]] int fd() const { return fd_; }

        // Reads what the socket has; false at its end (or an error), with the reason.
        bool read_available(sc::tcp_close_reason &reason, std::uint64_t &received) {
            char buffer[16384];
            bool got = false;
            while (true) {
                const auto n = ::recv(fd_, buffer, sizeof buffer, 0);
                if (n > 0) {
                    received += static_cast<std::uint64_t>(n);
                    got = true;
                    const std::lock_guard lock{input_mutex_};
                    arrived_.append(buffer, static_cast<std::size_t>(n));
                    if (arrived_.size() + working_size_.load() > options_.max_input) {
                        reason = sc::tcp_close_reason::input_overflow;
                        return false;
                    }
                    continue;
                }
                if (n == 0) {
                    reason = sc::tcp_close_reason::peer;
                    if (got) last_received_ = steady::now();
                    return false;
                }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                reason = errno == ECONNRESET ? sc::tcp_close_reason::peer : sc::tcp_close_reason::error;
                return false;
            }
            if (got) last_received_ = steady::now();
            return true;
        }

        // Writes what's waiting; true when it's all out.
        bool flush() {
            const std::lock_guard lock{output_mutex_};
            if (fd_ < 0) return true;
            while (!output_.empty()) {
                const auto n = write_some(fd_, output_.data(), output_.size());
                if (n > 0) {
                    sent_ += static_cast<std::uint64_t>(n);
                    output_.erase(0, static_cast<std::size_t>(n));
                } else if (n < 0 && errno == EINTR) {
                    continue;
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return false;
                } else {
                    output_.clear();
                    request_close(sc::tcp_close_reason::error);
                    return true;
                }
            }
            events_.want_write(fd_, false);
            return true;
        }

        [[nodiscard]] bool output_empty() {
            const std::lock_guard lock{output_mutex_};
            return output_.empty();
        }

        // Marks it closing (the first reason counts), and has the I/O thread close it.
        void request_close(const sc::tcp_close_reason reason) {
            bool expected = false;
            if (closing_.compare_exchange_strong(expected, true)) {
                close_reason_ = reason;
                wake_to_close_(id_);
            }
        }

        [[nodiscard]] bool closing() const { return closing_.load(); }
        void peer_gone() { gone_ = true; }        // the other end closed while output waited
        [[nodiscard]] bool gone() const { return gone_.load(); }
        [[nodiscard]] sc::tcp_close_reason close_reason() const { return close_reason_; }

        // Shuts the socket; the fd stays until the connection goes, so a late send() fails cleanly.
        void shut(poller &events) {
            const std::lock_guard lock{output_mutex_};
            if (fd_ < 0) return;
            events.remove(fd_);
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }

        [[nodiscard]] steady::time_point last_received() const { return last_received_; }
        steady::time_point &next_tick() { return next_tick_; }
        [[nodiscard]] std::uint64_t sent() const { return sent_.load(); }

        // --- the strand ---

        // Queues a task for this connection; true when the strand needs a worker.
        bool post(std::function<void()> task) {
            const std::lock_guard lock{tasks_mutex_};
            tasks_.push_back(std::move(task));
            if (scheduled_) return false;
            scheduled_ = true;
            return true;
        }

        // Runs its tasks until none are left (a worker's job).
        void drain() {
            while (true) {
                std::function<void()> task;
                {
                    const std::lock_guard lock{tasks_mutex_};
                    if (tasks_.empty()) {
                        scheduled_ = false;
                        return;
                    }
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                }
                task();
            }
        }

        // Moves what arrived onto the strand's own buffer, for the received handler; set only once
        // in the queue (data_task_queued_) however many reads come before it runs.
        std::string &take_arrived() {
            {
                const std::lock_guard lock{input_mutex_};
                working_.append(arrived_);
                arrived_.clear();
                data_task_queued_ = false;
            }
            working_size_ = working_.size();
            return working_;
        }

        void consumed(const std::size_t count) {
            working_.erase(0, std::min(count, working_.size()));
            working_size_ = working_.size();
        }

        // Whether a received task is already waiting (then a new read needn't queue another).
        bool claim_data_task() {
            const std::lock_guard lock{input_mutex_};
            if (data_task_queued_) return false;
            data_task_queued_ = true;
            return true;
        }

    private:
        std::uint64_t id_;
        int fd_;
        sc::ip_endpoint remote_, local_;
        poller &events_;
        const sc::tcp_server_options &options_;
        std::function<void(std::uint64_t)> wake_to_close_;
        std::chrono::system_clock::time_point connected_at_;
        std::any context_;

        std::mutex output_mutex_;
        std::string output_;
        std::atomic<std::uint64_t> sent_{0};
        std::atomic<bool> closing_{false};
        std::atomic<bool> gone_{false};
        sc::tcp_close_reason close_reason_ = sc::tcp_close_reason::closed;

        std::mutex input_mutex_;
        std::string arrived_;
        bool data_task_queued_ = false;
        std::string working_;              // the strand's alone
        std::atomic<std::size_t> working_size_{0};

        steady::time_point last_received_; // the I/O thread's
        steady::time_point next_tick_;     // the I/O thread's

        std::mutex tasks_mutex_;
        std::deque<std::function<void()>> tasks_;
        bool scheduled_ = false;
    };

    // Workers taking connections whose strand has tasks.
    class worker_pool {
    public:
        explicit worker_pool(std::size_t count) {
            if (count == 0) count = std::max<std::size_t>(2, std::thread::hardware_concurrency());
            for (std::size_t i = 0; i < count; ++i) threads_.emplace_back([this] { work(); });
        }

        ~worker_pool() { finish(); }

        void schedule(std::shared_ptr<connection> c) {
            {
                const std::lock_guard lock{mutex_};
                queue_.push_back(std::move(c));
            }
            ready_.notify_one();
        }

        // Runs what's queued, then stops the workers.
        void finish() {
            {
                const std::lock_guard lock{mutex_};
                if (stopping_) return;
                stopping_ = true;
            }
            ready_.notify_all();
            for (auto &t: threads_)
                if (t.joinable()) t.join();
        }

    private:
        void work() {
            while (true) {
                std::shared_ptr<connection> next;
                {
                    std::unique_lock lock{mutex_};
                    ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                    if (queue_.empty()) return; // stopping, and nothing left
                    next = std::move(queue_.front());
                    queue_.pop_front();
                }
                next->drain();
            }
        }

        std::mutex mutex_;
        std::condition_variable ready_;
        std::deque<std::shared_ptr<connection>> queue_;
        std::vector<std::thread> threads_;
        bool stopping_ = false;
    };
}

class sc::tcp_server::implementation {
public:
    implementation(ip_endpoints listen, tcp_handlers handlers, tcp_server_options options)
        : endpoints_(std::move(listen)), handlers_(std::move(handlers)), options_(options) {
        if (endpoints_.empty()) throw std::invalid_argument("sc-tcp-server: nothing to listen on");
        if (pipe(wake_) != 0) throw std::runtime_error(std::string{"sc-tcp-server: no pipe: "} + std::strerror(errno));
        nonblocking(wake_[0]);
        nonblocking(wake_[1]);
    }

    ~implementation() {
        stop();
        ::close(wake_[0]);
        ::close(wake_[1]);
    }

    void run() {
        {
            const std::lock_guard lock{state_mutex_};
            if (running_) throw std::runtime_error("sc-tcp-server: already running");
            running_ = true;
            stop_requested_ = false;
            failed_ = false;
        }
        poller events;
        worker_pool workers{options_.threads};
        std::vector<int> listeners;
        try {
            for (const auto &endpoint: endpoints_) listeners.push_back(bind_listener(endpoint));
        } catch (...) {
            for (const int fd: listeners) ::close(fd);
            {
                const std::lock_guard lock{state_mutex_};
                running_ = false;
                failed_ = true;
            }
            state_changed_.notify_all();
            throw;
        }
        {
            const std::lock_guard lock{state_mutex_};
            listening_.clear();
            for (const int fd: listeners) listening_.push_back(ip_endpoint::local_of(fd));
            accepting_ = true;
        }
        state_changed_.notify_all();
        for (const int fd: listeners) events.add(fd);
        events.add(wake_[0]);

        std::unordered_map<int, std::shared_ptr<connection>> by_fd;
        const auto schedule = [&workers](const std::shared_ptr<connection> &c, std::function<void()> task) {
            if (c->post(std::move(task))) workers.schedule(c);
        };
        // The disconnected handler, last in the strand; the connection goes once it has run.
        const auto finish = [&](const std::shared_ptr<connection> &c) {
            c->shut(events); // the callers took it out of by_fd first, while its fd was known
            {
                const std::lock_guard lock{connections_mutex_};
                connections_.erase(c->id());
                stats_.connections = connections_.size();
                stats_.bytes_sent += c->sent();
            }
            const auto reason = c->close_reason();
            schedule(c, [this, c, reason] { call([&] {
                if (handlers_.disconnected) handlers_.disconnected(*c, reason);
            }, *c, false); });
        };
        const auto close_now = [&](const std::shared_ptr<connection> &c, const tcp_close_reason reason) {
            c->request_close(reason);
            by_fd.erase(c->fd());
            finish(c);
        };

        while (!stop_requested_) {
            const auto now = steady::now();
            const auto waited = events.wait(std::chrono::milliseconds{100});
            for (const auto &event: waited) {
                if (event.fd == wake_[0]) {
                    char drain[256];
                    while (::read(wake_[0], drain, sizeof drain) > 0) {
                    }
                    continue;
                }
                if (std::ranges::find(listeners, event.fd) != listeners.end()) {
                    accept_all(event.fd, events, by_fd, schedule);
                    continue;
                }
                const auto found = by_fd.find(event.fd);
                if (found == by_fd.end()) continue;
                const auto c = found->second;
                if (event.writable) c->flush();
                if (event.readable && c->closing()) { // waiting for its output: drop what comes, note the end
                    tcp_close_reason ignored{};
                    std::uint64_t received = 0;
                    if (!c->read_available(ignored, received)) c->peer_gone();
                    continue;
                }
                if (event.readable && !c->closing()) {
                    tcp_close_reason reason{};
                    std::uint64_t received = 0;
                    const bool more = c->read_available(reason, received);
                    {
                        const std::lock_guard lock{connections_mutex_};
                        stats_.bytes_received += received;
                    }
                    if (received > 0 && c->claim_data_task()) {
                        schedule(c, [this, c] { call([&] {
                            if (!handlers_.received) return c->consumed(c->take_arrived().size());
                            auto &input = c->take_arrived();
                            while (!input.empty()) {
                                const auto used = handlers_.received(*c, std::string_view{input});
                                if (used == 0) break;
                                c->consumed(used);
                            }
                        }, *c, true); });
                    }
                    if (!more) c->request_close(reason);
                }
            }

            // Closes asked for (by send, close or a read): once what was sent is out, or at once
            // for anything but close().
            std::vector<std::uint64_t> to_close;
            {
                const std::lock_guard lock{close_mutex_};
                to_close.swap(close_queue_);
            }
            for (const auto id: to_close) {
                std::shared_ptr<connection> c;
                {
                    const std::lock_guard lock{connections_mutex_};
                    const auto found = connections_.find(id);
                    if (found != connections_.end()) c = std::static_pointer_cast<connection>(found->second);
                }
                if (!c) continue;
                if (c->close_reason() == tcp_close_reason::closed && !c->output_empty() && c->fd() >= 0) {
                    lingering_.push_back({c, now + std::chrono::seconds{5}});
                    continue;
                }
                by_fd.erase(c->fd());
                finish(c);
            }
            for (auto it = lingering_.begin(); it != lingering_.end();) { // close() waits for its output, up to 5 s
                if (it->first->output_empty() || it->first->gone() || now >= it->second) {
                    by_fd.erase(it->first->fd());
                    finish(it->first);
                    it = lingering_.erase(it);
                } else {
                    ++it;
                }
            }

            // Idle connections, and ticks.
            if (options_.idle_timeout.count() > 0 || options_.tick.count() > 0) {
                std::vector<std::shared_ptr<connection>> idle;
                for (auto &[fd, c]: by_fd) {
                    if (c->closing()) continue;
                    if (options_.idle_timeout.count() > 0 && now - c->last_received() >= options_.idle_timeout) {
                        idle.push_back(c);
                        continue;
                    }
                    if (options_.tick.count() > 0 && now >= c->next_tick()) {
                        c->next_tick() = now + options_.tick;
                        schedule(c, [this, c] { call([&] {
                            if (handlers_.tick) handlers_.tick(*c);
                        }, *c, true); });
                    }
                }
                for (const auto &c: idle) close_now(c, tcp_close_reason::idle);
            }
        }

        // Stopping: no more accepts; every connection closed and its disconnected run.
        for (const int fd: listeners) {
            events.remove(fd);
            ::close(fd);
        }
        {
            const std::lock_guard lock{state_mutex_};
            accepting_ = false;
        }
        std::vector<std::shared_ptr<connection>> left;
        for (auto &[fd, c]: by_fd) left.push_back(c);
        for (const auto &[c, deadline]: lingering_) left.push_back(c);
        lingering_.clear();
        for (const auto &c: left) {
            c->request_close(tcp_close_reason::server_stopped);
            by_fd.erase(c->fd());
            finish(c);
        }
        workers.finish(); // the queued handlers, disconnected among them, run before it returns
        {
            const std::lock_guard lock{close_mutex_};
            close_queue_.clear();
        }
        {
            const std::lock_guard lock{state_mutex_};
            running_ = false;
        }
        state_changed_.notify_all();
    }

    void stop() {
        stop_requested_ = true;
        wake();
    }

    bool wait_until_running(const std::chrono::milliseconds timeout) {
        std::unique_lock lock{state_mutex_};
        state_changed_.wait_for(lock, timeout, [this] { return accepting_ || failed_; });
        return accepting_;
    }

    std::vector<ip_endpoint> listening() const {
        const std::lock_guard lock{state_mutex_};
        return listening_;
    }

    tcp_server_stats stats() const {
        const std::lock_guard lock{connections_mutex_};
        auto out = stats_;
        out.connections = connections_.size();
        for (const auto &[id, c]: connections_) out.bytes_sent += std::static_pointer_cast<connection>(c)->sent();
        return out;
    }

    std::vector<std::shared_ptr<tcp_connection>> connections() const {
        const std::lock_guard lock{connections_mutex_};
        std::vector<std::shared_ptr<tcp_connection>> out;
        out.reserve(connections_.size());
        for (const auto &[id, c]: connections_) out.push_back(c);
        return out;
    }

private:
    // A handler, with what it throws closing the connection; a received or tick handler isn't
    // called once the connection is closing.
    template<typename Work>
    void call(Work work, connection &c, const bool only_while_open) {
        if (only_while_open && c.closing()) return;
        try {
            work();
        } catch (const std::exception &error) {
            warn("connection " + c.remote().to_string() + ": a handler threw: " + error.what());
            c.request_close(tcp_close_reason::error);
        } catch (...) {
            warn("connection " + c.remote().to_string() + ": a handler threw");
            c.request_close(tcp_close_reason::error);
        }
    }

    void wake() {
        const char byte = 1;
        [[maybe_unused]] const auto ignored = ::write(wake_[1], &byte, 1);
    }

    int bind_listener(const ip_endpoint &endpoint) {
        std::vector<socket_address> addresses;
        try {
            addresses = endpoint.resolve(ip_use::listen);
        } catch (const std::runtime_error &error) {
            throw std::runtime_error(std::string{"sc-tcp-server: "} + error.what());
        }
        std::string failure = "no address";
        int fd = -1;
        for (const auto &address: addresses) {
            fd = socket(address.family(), address.socket_type(), address.protocol());
            if (fd < 0) continue;
            const int yes = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
            if (address.ipv6()) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof yes);
            if (bind(fd, address.get(), address.size()) != 0 || listen(fd, options_.backlog) != 0 || !nonblocking(fd)) {
                failure = std::strerror(errno);
                ::close(fd);
                fd = -1;
                continue;
            }
            break;
        }
        if (fd < 0) throw std::runtime_error("sc-tcp-server: can't listen on " + endpoint.to_string() + ": " + failure);
        return fd;
    }

    template<typename Schedule>
    void accept_all(const int listener, poller &events, std::unordered_map<int, std::shared_ptr<connection>> &by_fd,
                    const Schedule &schedule) {
        while (true) {
            socket_address address;
            const int fd = ::accept(listener, address.get(), address.size_pointer());
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) warn(std::string{"accept: "} + std::strerror(errno));
                return;
            }
            {
                const std::lock_guard lock{connections_mutex_};
                if (connections_.size() >= options_.max_connections) {
                    ++stats_.refused;
                    ::close(fd);
                    warn("refused a connection: " + std::to_string(options_.max_connections) + " are open (max_connections)");
                    continue;
                }
            }
            nonblocking(fd);
            const int yes = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
#ifdef SO_NOSIGPIPE
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof yes);
            const auto c = std::make_shared<connection>(++next_id_, fd, ip_endpoint::from(address), ip_endpoint::local_of(fd), events, options_,
                                                        [this](const std::uint64_t id) {
                                                            {
                                                                const std::lock_guard lock{close_mutex_};
                                                                close_queue_.push_back(id);
                                                            }
                                                            wake();
                                                        });
            {
                const std::lock_guard lock{connections_mutex_};
                connections_[c->id()] = c;
                ++stats_.accepted;
                stats_.connections = connections_.size();
            }
            by_fd[fd] = c;
            schedule(c, [this, c] { call([&] {
                if (handlers_.connected) handlers_.connected(*c);
            }, *c, false); });
            events.add(fd);
        }
    }

    ip_endpoints endpoints_;
    tcp_handlers handlers_;
    tcp_server_options options_;
    int wake_[2]{-1, -1};

    mutable std::mutex state_mutex_;
    std::condition_variable state_changed_;
    bool running_ = false, accepting_ = false, failed_ = false;
    std::atomic<bool> stop_requested_{false};
    std::vector<ip_endpoint> listening_;

    mutable std::mutex connections_mutex_;
    std::map<std::uint64_t, std::shared_ptr<tcp_connection>> connections_;
    tcp_server_stats stats_;
    std::atomic<std::uint64_t> next_id_{0};

    std::mutex close_mutex_;
    std::vector<std::uint64_t> close_queue_;
    std::vector<std::pair<std::shared_ptr<connection>, steady::time_point>> lingering_;
};

sc::tcp_server::tcp_server(ip_endpoints listen, tcp_handlers handlers, tcp_server_options options)
    : implementation_(std::make_unique<implementation>(std::move(listen), std::move(handlers), options)) {
}

sc::tcp_server::~tcp_server() = default;

void sc::tcp_server::run() {
    implementation_->run();
}

void sc::tcp_server::stop() {
    implementation_->stop();
}

bool sc::tcp_server::wait_until_running(const std::chrono::milliseconds timeout) {
    return implementation_->wait_until_running(timeout);
}

std::vector<sc::ip_endpoint> sc::tcp_server::listening() const {
    return implementation_->listening();
}

sc::tcp_server_stats sc::tcp_server::stats() const {
    return implementation_->stats();
}

std::vector<std::shared_ptr<sc::tcp_connection>> sc::tcp_server::connections() const {
    return implementation_->connections();
}
