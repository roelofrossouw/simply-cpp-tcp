#include "tcp_connection_state.h"

#include <tcp_connection.h>

#include <algorithm>
#include <cerrno>

namespace sc {
    std::string_view tcp_close_reason_name(const tcp_close_reason reason) {
        switch (reason) {
            case tcp_close_reason::peer: return "peer";
            case tcp_close_reason::closed: return "closed";
            case tcp_close_reason::idle: return "idle";
            case tcp_close_reason::error: return "error";
            case tcp_close_reason::input_overflow: return "input_overflow";
            case tcp_close_reason::output_overflow: return "output_overflow";
            case tcp_close_reason::stopped: return "stopped";
        }
        return "unknown";
    }

    tcp_connection::tcp_connection(std::shared_ptr<tcp::connection_state> state) : state_(std::move(state)) {
    }

    tcp_connection::~tcp_connection() = default;

    std::uint64_t tcp_connection::id() const { return state_->id; }
    const ip_endpoint &tcp_connection::remote() const { return state_->remote; }
    const ip_endpoint &tcp_connection::local() const { return state_->local; }
    std::chrono::system_clock::time_point tcp_connection::connected_at() const { return state_->connected_at; }
    bool tcp_connection::open() const { return !state_->closing(); }
    bool tcp_connection::send(const std::string_view bytes) { return state_->send(bytes); }
    void tcp_connection::close() { state_->close(); }
    std::any &tcp_connection::context() { return state_->context; }
}

namespace sc::tcp {
    connection_state::connection_state(const std::uint64_t id, socket s, ip_endpoint remote, ip_endpoint local, poller &events,
                                       const connection_limits &limits, std::function<void(std::uint64_t)> wake_to_close)
        : id(id), remote(std::move(remote)), local(std::move(local)), next_tick(steady::now() + limits.tick), socket_(std::move(s)),
          fd_(socket_.fd()), events_(events), limits_(limits), wake_to_close_(std::move(wake_to_close)) {
    }

    bool connection_state::send(const std::string_view bytes) {
        const std::lock_guard lock{output_mutex_};
        if (closing_ || !socket_.valid()) return false;
        std::size_t offset = 0;
        if (output_.empty()) { // straight out, as far as the socket takes it
            while (offset < bytes.size()) {
                const auto n = socket_.write_some(bytes.data() + offset, bytes.size() - offset);
                if (n > 0) {
                    offset += static_cast<std::size_t>(n);
                    sent_ += static_cast<std::uint64_t>(n);
                } else if (n < 0 && errno == EINTR) {
                    continue;
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                } else {
                    request_close(tcp_close_reason::error);
                    return false;
                }
            }
        }
        if (offset < bytes.size()) {
            if (output_.size() + bytes.size() - offset > limits_.max_output) {
                request_close(tcp_close_reason::output_overflow);
                return false;
            }
            const bool was_empty = output_.empty();
            output_.append(bytes.substr(offset));
            if (was_empty) events_.want_write(fd_, true);
        }
        return true;
    }

    void connection_state::close() {
        const std::lock_guard lock{output_mutex_};
        request_close(tcp_close_reason::closed);
    }

    bool connection_state::read_available(tcp_close_reason &reason, std::uint64_t &received) {
        char buffer[16384];
        bool got = false;
        while (true) {
            const auto n = socket_.read_some(buffer, sizeof buffer);
            if (n > 0) {
                received += static_cast<std::uint64_t>(n);
                got = true;
                const std::lock_guard lock{input_mutex_};
                arrived_.append(buffer, static_cast<std::size_t>(n));
                if (arrived_.size() + working_size_.load() > limits_.max_input) {
                    reason = tcp_close_reason::input_overflow;
                    return false;
                }
                continue;
            }
            if (n == 0) {
                reason = tcp_close_reason::peer;
                if (got) last_received = steady::now();
                return false;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            reason = errno == ECONNRESET ? tcp_close_reason::peer : tcp_close_reason::error;
            return false;
        }
        if (got) last_received = steady::now();
        return true;
    }

    bool connection_state::flush() {
        const std::lock_guard lock{output_mutex_};
        if (!socket_.valid()) return true;
        while (!output_.empty()) {
            const auto n = socket_.write_some(output_.data(), output_.size());
            if (n > 0) {
                sent_ += static_cast<std::uint64_t>(n);
                output_.erase(0, static_cast<std::size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return false;
            } else {
                output_.clear();
                request_close(tcp_close_reason::error);
                return true;
            }
        }
        events_.want_write(fd_, false);
        return true;
    }

    bool connection_state::output_empty() {
        const std::lock_guard lock{output_mutex_};
        return output_.empty();
    }

    void connection_state::request_close(const tcp_close_reason reason) {
        bool expected = false;
        if (closing_.compare_exchange_strong(expected, true)) {
            close_reason_ = reason;
            wake_to_close_(id);
        }
    }

    void connection_state::shut() {
        const std::lock_guard lock{output_mutex_};
        if (!socket_.valid()) return;
        events_.remove(socket_.fd());
        socket_.shutdown();
        socket_.close();
    }

    std::string &connection_state::take_arrived() {
        {
            const std::lock_guard lock{input_mutex_};
            working_.append(arrived_);
            arrived_.clear();
            data_task_queued_ = false;
        }
        working_size_ = working_.size();
        return working_;
    }

    void connection_state::consumed(const std::size_t count) {
        working_.erase(0, std::min(count, working_.size()));
        working_size_ = working_.size();
    }

    bool connection_state::claim_data_task() {
        const std::lock_guard lock{input_mutex_};
        if (data_task_queued_) return false;
        data_task_queued_ = true;
        return true;
    }
}
