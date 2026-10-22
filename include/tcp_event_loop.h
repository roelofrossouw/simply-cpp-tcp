#pragma once

#include "tcp_connection_state.h"
#include "tcp_poller.h"
#include "tcp_socket.h"
#include "tcp_worker_pool.h"

#include <tcp_connection.h>
#include <tcp_handlers.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The I/O thread: one loop waiting on every socket, which tcp_server (listeners and the
// connections they accept) and tcp_client (connections it makes) are built on. Handlers run on the
// worker pool, on each connection's strand. For a program that wants both, or its own front end.
namespace sc::tcp {
    struct loop_options {
        std::size_t threads = 0;
        std::chrono::milliseconds idle_timeout{0};
        std::size_t max_connections = 10000;
        connection_limits limits;
    };

    class event_loop {
    public:
        using connected_callback = std::function<void(const std::shared_ptr<tcp_connection> &)>;
        using failed_callback = std::function<void(const std::string &why)>;
        using closed_callback = std::function<void(const std::shared_ptr<tcp_connection> &, tcp_close_reason)>;

        event_loop(tcp_handlers handlers, loop_options options);
        ~event_loop();
        event_loop(const event_loop &) = delete;
        event_loop &operator=(const event_loop &) = delete;

        // A listening socket whose connections it accepts. Before run().
        void listen(socket listener);
        // Connects to endpoint (each of its addresses in turn, timeout each); on the I/O thread,
        // connected with the new connection (its connected handler queued), or failed with why.
        // From any thread.
        void connect(const ip_endpoint &endpoint, std::chrono::milliseconds timeout, connected_callback connected,
                     failed_callback failed);
        // Runs fn on the I/O thread after delay. From any thread.
        void after(std::chrono::milliseconds delay, std::function<void()> fn);
        // Called on the I/O thread when a connection has closed (after queueing its disconnected).
        void on_closed(closed_callback closed) { on_closed_ = std::move(closed); }

        // Serves until stop(); started runs on the I/O thread once it's waiting.
        void run(const std::function<void()> &started = {});
        void stop();
        [[nodiscard]] bool stopping() const { return stop_requested_.load(); }

        [[nodiscard]] std::vector<ip_endpoint> listening() const;
        [[nodiscard]] tcp_stats stats() const;
        [[nodiscard]] std::vector<std::shared_ptr<tcp_connection>> connections() const;
        void count_refused();

    private:
        struct pending_connect;

        void wake();
        void accept_all(const socket &listener);
        std::shared_ptr<tcp_connection> add_connection(socket s, ip_endpoint remote);
        void finish(const std::shared_ptr<tcp_connection> &c);
        void process_closes(steady::time_point now);
        void process_ticks(steady::time_point now);
        void process_connects(steady::time_point now);
        void process_timers(steady::time_point now);
        void try_next_address(const std::shared_ptr<pending_connect> &p);
        template<typename Work>
        void call(Work work, const std::shared_ptr<tcp_connection> &c, bool only_while_open);

        static connection_state &state_of(const tcp_connection &c);

        tcp_handlers handlers_;
        loop_options options_;
        poller events_;
        std::unique_ptr<worker_pool> workers_;
        int wake_[2]{-1, -1};
        std::atomic<bool> stop_requested_{false};

        std::vector<socket> listeners_;
        std::vector<ip_endpoint> listening_;
        std::unordered_map<int, std::shared_ptr<tcp_connection>> by_fd_; // the I/O thread's
        std::vector<std::pair<std::shared_ptr<tcp_connection>, steady::time_point>> lingering_;

        mutable std::mutex connections_mutex_;
        std::map<std::uint64_t, std::shared_ptr<tcp_connection>> connections_;
        tcp_stats stats_;
        std::atomic<std::uint64_t> next_id_{0};

        std::mutex close_mutex_;
        std::vector<std::uint64_t> close_queue_;

        std::mutex requests_mutex_; // connects and timers asked for from other threads
        std::vector<std::shared_ptr<pending_connect>> connect_requests_;
        std::vector<std::pair<steady::time_point, std::function<void()>>> timers_;
        std::unordered_map<int, std::shared_ptr<pending_connect>> connecting_; // the I/O thread's, by fd

        closed_callback on_closed_;
    };

    void warn(const std::string &text);
}
