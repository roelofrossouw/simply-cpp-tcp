#pragma once

#include "tcp_poller.h"
#include "tcp_socket.h"
#include "tcp_worker_pool.h"

#include <sc/tcp/connection.h>

#include <any>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

// What a tcp::connection is underneath: its socket, the input the I/O thread reads and the strand's
// handlers consume, the output waiting for the socket, and its handlers' strand. The event loop's;
// programs use tcp::connection.
namespace sc::tcp {
    using steady = std::chrono::steady_clock;

    struct connection_limits {
        std::size_t max_input = 1 << 20;
        std::size_t max_output = 4 << 20;
        std::chrono::milliseconds tick{0};
    };

    struct connection_state final : strand {
        connection_state(std::uint64_t id, socket s, ip_endpoint remote, ip_endpoint local, poller &events,
                         const connection_limits &limits, std::function<void(std::uint64_t)> wake_to_close);

        // --- tcp::connection's, from any thread ---
        bool send(std::string_view bytes);
        void close();

        // --- the I/O thread's ---
        // Reads what the socket has; false at its end (or an error), with the reason.
        bool read_available(tcp::close_reason &reason, std::uint64_t &received);
        // Writes what's waiting; true when it's all out.
        bool flush();
        [[nodiscard]] bool output_empty();
        // Marks it closing (the first reason counts) and has the I/O thread close it.
        void request_close(tcp::close_reason reason);
        // Shuts the socket; the state stays until the connection goes, so a late send() fails cleanly.
        void shut();

        // --- the strand's ---
        // Moves what arrived onto the strand's own buffer for the received handler.
        std::string &take_arrived();
        void consumed(std::size_t count);
        // Whether a received task needs queueing (one waiting takes what arrives before it runs).
        bool claim_data_task();

        const std::uint64_t id;
        const ip_endpoint remote, local;
        const std::chrono::system_clock::time_point connected_at = std::chrono::system_clock::now();
        std::any context;

        [[nodiscard]] int fd() const { return fd_; }
        [[nodiscard]] bool closing() const { return close_reason_.load() >= 0; }
        [[nodiscard]] tcp::close_reason close_reason() const {
            const auto reason = close_reason_.load();
            return reason < 0 ? tcp::close_reason::closed : static_cast<tcp::close_reason>(reason);
        }
        // Whether input still goes to the received handler: open, or closed by the peer (what it
        // sent before it closed is still read).
        [[nodiscard]] bool reading() const {
            const auto reason = close_reason_.load();
            return reason < 0 || reason == static_cast<int>(tcp::close_reason::peer);
        }
        void peer_gone() { gone_ = true; } // the other end closed while output waited
        [[nodiscard]] bool gone() const { return gone_.load(); }
        [[nodiscard]] std::uint64_t sent() const { return sent_.load(); }
        steady::time_point last_received = steady::now(); // the I/O thread's
        steady::time_point next_tick;                       // the I/O thread's

    private:
        socket socket_;
        int fd_; // socket_'s, kept for the I/O thread's maps after shut()
        poller &events_;
        const connection_limits &limits_;
        std::function<void(std::uint64_t)> wake_to_close_;

        std::mutex output_mutex_;
        std::string output_;
        std::atomic<std::uint64_t> sent_{0};
        std::atomic<bool> gone_{false};
        std::atomic<int> close_reason_{-1}; // a tcp::close_reason once closing (the first one counts); -1 while open

        std::mutex input_mutex_;
        std::string arrived_;
        bool data_task_queued_ = false;
        std::string working_; // the strand's alone
        std::atomic<std::size_t> working_size_{0};
    };
}
