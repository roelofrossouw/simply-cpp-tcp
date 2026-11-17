#include "tcp_event_loop.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace sc::tcp {
    namespace {
        std::atomic<sc::log_level> level{sc::log_level::warning};
    }

    void warn(const std::string &text) {
        if (level.load() >= sc::log_level::warning) std::cerr << "sc-tcp: " << text << std::endl;
    }

    struct event_loop::pending_connect {
        ip_endpoint endpoint;
        std::chrono::milliseconds timeout;
        connected_callback connected;
        failed_callback failed;
        std::vector<socket_address> addresses;
        std::size_t next = 0;
        socket attempt;
        steady::time_point deadline;
        std::string last_error = "no address";
    };

    event_loop::event_loop(tcp_handlers handlers, loop_options options)
        : handlers_(std::move(handlers)), options_(options) {
        if (pipe(wake_) != 0) throw std::runtime_error(std::string{"sc-tcp: no pipe: "} + std::strerror(errno));
        nonblocking(wake_[0]);
        nonblocking(wake_[1]);
    }

    event_loop::~event_loop() {
        ::close(wake_[0]);
        ::close(wake_[1]);
    }

    connection_state &event_loop::state_of(const tcp_connection &c) {
        return *c.state_;
    }

    void event_loop::listen(socket listener) {
        listening_.push_back(ip_endpoint::local_of(listener.fd()));
        listeners_.push_back(std::move(listener));
    }

    void event_loop::connect(const ip_endpoint &endpoint, const std::chrono::milliseconds timeout, connected_callback connected,
                             failed_callback failed) {
        auto p = std::make_shared<pending_connect>();
        p->endpoint = endpoint;
        p->timeout = timeout;
        p->connected = std::move(connected);
        p->failed = std::move(failed);
        {
            const std::lock_guard lock{requests_mutex_};
            connect_requests_.push_back(std::move(p));
        }
        wake();
    }

    void event_loop::after(const std::chrono::milliseconds delay, std::function<void()> fn) {
        {
            const std::lock_guard lock{requests_mutex_};
            timers_.emplace_back(steady::now() + delay, std::move(fn));
        }
        wake();
    }

    void event_loop::wake() {
        const char byte = 1;
        [[maybe_unused]] const auto ignored = ::write(wake_[1], &byte, 1);
    }

    void event_loop::stop() {
        stop_requested_ = true;
        wake();
    }

    void event_loop::count_refused() {
        const std::lock_guard lock{connections_mutex_};
        ++stats_.refused;
    }

    // A handler, with what it throws closing the connection; received and tick aren't called once
    // the connection is closing.
    template<typename Work>
    void event_loop::call(Work work, const std::shared_ptr<tcp_connection> &c, const bool only_while_open) {
        auto &state = state_of(*c);
        if (only_while_open && state.closing()) return;
        try {
            work();
        } catch (const std::exception &error) {
            warn("connection " + c->remote().to_string() + ": a handler threw: " + error.what());
            state.request_close(tcp_close_reason::error);
        } catch (...) {
            warn("connection " + c->remote().to_string() + ": a handler threw");
            state.request_close(tcp_close_reason::error);
        }
    }

    std::shared_ptr<tcp_connection> event_loop::add_connection(socket s, ip_endpoint remote) {
        const int fd = s.fd();
        auto local = ip_endpoint::local_of(fd);
        auto state = std::make_shared<connection_state>(++next_id_, std::move(s), std::move(remote), std::move(local), events_,
                                                        options_.limits, [this](const std::uint64_t id) {
                                                            {
                                                                const std::lock_guard lock{close_mutex_};
                                                                close_queue_.push_back(id);
                                                            }
                                                            wake();
                                                        });
        std::shared_ptr<tcp_connection> c{new tcp_connection{state}};
        {
            const std::lock_guard lock{connections_mutex_};
            connections_[state->id] = c;
            ++stats_.accepted;
            stats_.connections = connections_.size();
        }
        by_fd_[fd] = c;
        workers_->post(state, [this, c] { call([&] {
            if (handlers_.connected) handlers_.connected(*c);
        }, c, false); });
        events_.add(fd);
        return c;
    }

    void event_loop::accept_all(const socket &listener) {
        while (true) {
            socket_address address;
            socket accepted = socket::accept_from(listener, address);
            if (!accepted.valid()) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) warn(std::string{"accept: "} + std::strerror(errno));
                return;
            }
            {
                const std::lock_guard lock{connections_mutex_};
                if (connections_.size() >= options_.max_connections) {
                    ++stats_.refused;
                    warn("refused a connection: " + std::to_string(options_.max_connections) + " are open (max_connections)");
                    continue; // accepted closes as it goes
                }
            }
            add_connection(std::move(accepted), ip_endpoint::from(address));
        }
    }

    // The disconnected handler, last on the strand; the connection goes once it has run. The
    // caller took it out of by_fd_ first, while its fd was known.
    void event_loop::finish(const std::shared_ptr<tcp_connection> &c) {
        auto &state = state_of(*c);
        state.shut();
        {
            const std::lock_guard lock{connections_mutex_};
            connections_.erase(state.id);
            stats_.connections = connections_.size();
            stats_.bytes_sent += state.sent();
        }
        const auto reason = state.close_reason();
        workers_->post(c->state_, [this, c, reason] { call([&] {
            if (handlers_.disconnected) handlers_.disconnected(*c, reason);
        }, c, false); });
        if (on_closed_) on_closed_(c, reason);
    }

    // Closes asked for (by send, close or a read): close() waits for its output, up to 5 s.
    void event_loop::process_closes(const steady::time_point now) {
        std::vector<std::uint64_t> to_close;
        {
            const std::lock_guard lock{close_mutex_};
            to_close.swap(close_queue_);
        }
        for (const auto id: to_close) {
            std::shared_ptr<tcp_connection> c;
            {
                const std::lock_guard lock{connections_mutex_};
                if (const auto found = connections_.find(id); found != connections_.end()) c = found->second;
            }
            if (!c) continue;
            auto &state = state_of(*c);
            if (state.close_reason() == tcp_close_reason::closed && !state.output_empty()) {
                lingering_.emplace_back(c, now + std::chrono::seconds{5});
                continue;
            }
            by_fd_.erase(state.fd());
            finish(c);
        }
        for (auto it = lingering_.begin(); it != lingering_.end();) {
            auto &state = state_of(*it->first);
            if (state.output_empty() || state.gone() || now >= it->second) {
                by_fd_.erase(state.fd());
                finish(it->first);
                it = lingering_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Idle connections, and ticks.
    void event_loop::process_ticks(const steady::time_point now) {
        const auto tick = options_.limits.tick;
        if (options_.idle_timeout.count() <= 0 && tick.count() <= 0) return;
        std::vector<std::shared_ptr<tcp_connection>> idle;
        for (auto &[fd, c]: by_fd_) {
            auto &state = state_of(*c);
            if (state.closing()) continue;
            if (options_.idle_timeout.count() > 0 && now - state.last_received >= options_.idle_timeout) {
                idle.push_back(c);
                continue;
            }
            if (tick.count() > 0 && now >= state.next_tick) {
                state.next_tick = now + tick;
                workers_->post(c->state_, [this, c] { call([&] {
                    if (handlers_.tick) handlers_.tick(*c);
                }, c, true); });
            }
        }
        for (const auto &c: idle) {
            auto &state = state_of(*c);
            state.request_close(tcp_close_reason::idle);
            by_fd_.erase(state.fd());
            finish(c);
        }
    }

    // Starts the next address of a connect; fails it when none are left.
    void event_loop::try_next_address(const std::shared_ptr<pending_connect> &p) {
        while (p->next < p->addresses.size()) {
            bool in_progress = false;
            p->attempt = socket::connect_to(p->addresses[p->next++], in_progress);
            if (!p->attempt.valid()) {
                p->last_error = std::strerror(errno);
                continue;
            }
            if (!in_progress) { // connected at once (a local address)
                auto remote = ip_endpoint::peer_of(p->attempt.fd());
                const auto c = add_connection(std::move(p->attempt), std::move(remote));
                if (p->connected) p->connected(c);
                return;
            }
            p->deadline = steady::now() + p->timeout;
            connecting_[p->attempt.fd()] = p;
            events_.add(p->attempt.fd());
            events_.want_write(p->attempt.fd(), true);
            return;
        }
        count_refused();
        if (p->failed) p->failed(p->endpoint.to_string() + ": " + p->last_error);
    }

    void event_loop::process_connects(const steady::time_point now) {
        std::vector<std::shared_ptr<pending_connect>> requests;
        {
            const std::lock_guard lock{requests_mutex_};
            requests.swap(connect_requests_);
        }
        for (const auto &p: requests) {
            try {
                p->addresses = p->endpoint.resolve(ip_use::connect);
            } catch (const std::exception &error) {
                count_refused();
                if (p->failed) p->failed(error.what());
                continue;
            }
            try_next_address(p);
        }
        std::vector<int> timed_out;
        for (const auto &[fd, p]: connecting_)
            if (now >= p->deadline) timed_out.push_back(fd);
        for (const int fd: timed_out) {
            const auto p = connecting_[fd];
            connecting_.erase(fd);
            events_.remove(fd);
            p->attempt.close();
            p->last_error = "no connection within " + std::to_string(p->timeout.count()) + " ms";
            try_next_address(p);
        }
    }

    void event_loop::process_timers(const steady::time_point now) {
        std::vector<std::function<void()>> due;
        {
            const std::lock_guard lock{requests_mutex_};
            for (auto it = timers_.begin(); it != timers_.end();) {
                if (now >= it->first) {
                    due.push_back(std::move(it->second));
                    it = timers_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto &fn: due) fn();
    }

    void event_loop::run(const std::function<void()> &started) {
        stop_requested_ = false;
        workers_ = std::make_unique<worker_pool>(options_.threads);
        for (const auto &l: listeners_) events_.add(l.fd());
        events_.add(wake_[0]);
        if (started) started();

        while (!stop_requested_) {
            const auto waited = events_.wait(std::chrono::milliseconds{100});
            const auto now = steady::now();
            for (const auto &event: waited) {
                if (event.fd == wake_[0]) {
                    char drain[256];
                    while (::read(wake_[0], drain, sizeof drain) > 0) {
                    }
                    continue;
                }
                if (const auto l = std::ranges::find_if(listeners_, [&](const socket &s) { return s.fd() == event.fd; });
                    l != listeners_.end()) {
                    accept_all(*l);
                    continue;
                }
                if (const auto pending = connecting_.find(event.fd); pending != connecting_.end()) {
                    if (!event.writable && !event.readable) continue;
                    const auto p = pending->second;
                    connecting_.erase(pending);
                    events_.remove(event.fd);
                    if (const int error = p->attempt.connect_error(); error != 0) {
                        p->attempt.close();
                        p->last_error = std::strerror(error);
                        try_next_address(p);
                        continue;
                    }
                    auto remote = ip_endpoint::peer_of(p->attempt.fd());
                    const auto c = add_connection(std::move(p->attempt), std::move(remote));
                    if (p->connected) p->connected(c);
                    continue;
                }
                const auto found = by_fd_.find(event.fd);
                if (found == by_fd_.end()) continue;
                const auto c = found->second;
                auto &state = state_of(*c);
                if (event.writable) state.flush();
                if (event.readable && state.closing()) { // waiting for its output: drop what comes, note the end
                    tcp_close_reason ignored{};
                    std::uint64_t received = 0;
                    if (!state.read_available(ignored, received)) state.peer_gone();
                    continue;
                }
                if (event.readable) {
                    tcp_close_reason reason{};
                    std::uint64_t received = 0;
                    const bool more = state.read_available(reason, received);
                    {
                        const std::lock_guard lock{connections_mutex_};
                        stats_.bytes_received += received;
                    }
                    if (received > 0 && state.claim_data_task()) {
                        workers_->post(c->state_, [this, c] { call([&] {
                            auto &s = state_of(*c);
                            auto &input = s.take_arrived();
                            if (!handlers_.received) return s.consumed(input.size());
                            while (!input.empty()) {
                                const auto used = handlers_.received(*c, std::string_view{input});
                                if (used == 0) break;
                                s.consumed(used);
                            }
                        }, c, true); });
                    }
                    if (!more) state.request_close(reason);
                }
            }
            process_connects(now);
            process_timers(now);
            process_closes(now);
            process_ticks(now);
        }

        // Stopping: no more accepts or connects; every connection closed and its disconnected run.
        for (auto &l: listeners_) {
            events_.remove(l.fd());
            l.close();
        }
        listeners_.clear();
        for (auto &[fd, p]: connecting_) {
            events_.remove(fd);
            p->attempt.close();
        }
        connecting_.clear();
        std::vector<std::shared_ptr<tcp_connection>> left;
        for (auto &[fd, c]: by_fd_) left.push_back(c);
        for (const auto &[c, deadline]: lingering_) left.push_back(c);
        lingering_.clear();
        for (const auto &c: left) {
            auto &state = state_of(*c);
            state.request_close(tcp_close_reason::stopped);
            by_fd_.erase(state.fd());
            finish(c);
        }
        workers_->finish(); // the queued handlers, disconnected among them, run before it returns
        {
            const std::lock_guard lock{close_mutex_};
            close_queue_.clear();
        }
        {
            const std::lock_guard lock{requests_mutex_};
            connect_requests_.clear();
            timers_.clear();
        }
    }

    std::vector<ip_endpoint> event_loop::listening() const {
        return listening_;
    }

    tcp_stats event_loop::stats() const {
        const std::lock_guard lock{connections_mutex_};
        auto out = stats_;
        out.connections = connections_.size();
        for (const auto &[id, c]: connections_) out.bytes_sent += c->state_->sent();
        return out;
    }

    std::vector<std::shared_ptr<tcp_connection>> event_loop::connections() const {
        const std::lock_guard lock{connections_mutex_};
        std::vector<std::shared_ptr<tcp_connection>> out;
        out.reserve(connections_.size());
        for (const auto &[id, c]: connections_) out.push_back(c);
        return out;
    }
}

void sc::tcp_log_level(const sc::log_level level) {
    tcp::level = level;
}
