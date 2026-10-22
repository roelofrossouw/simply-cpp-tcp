#include "tcp_event_loop.h"

#include <tcp_client.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>

class sc::tcp_client::implementation {
public:
    implementation(ip_endpoint server, tcp_handlers handlers, const tcp_client_options &options)
        : server_(std::move(server)), handlers_(std::move(handlers)), options_(options), delay_(options.reconnect) {
        if (server_.host.empty() || server_.port <= 0) throw std::invalid_argument("sc-tcp: a client needs a host and a port");
    }

    ~implementation() { stop(); }

    void run() {
        std::shared_ptr<tcp::event_loop> loop;
        {
            const std::lock_guard lock{mutex_};
            if (running_) throw std::runtime_error("sc-tcp: already running");
            running_ = true;
            ended_ = false;
            failure_.reset();
            loop_ = loop = std::make_shared<tcp::event_loop>(
                handlers_, tcp::loop_options{options_.threads, options_.idle_timeout, 1,
                                                    {options_.max_input, options_.max_output, options_.tick}});
        }
        loop->on_closed([this, loop](const std::shared_ptr<tcp_connection> &, const tcp_close_reason) {
            {
                const std::lock_guard lock{mutex_};
                connection_.reset();
            }
            changed_.notify_all();
            if (loop->stopping()) return;
            if (options_.reconnect.count() > 0) {
                loop->after(delay_, [this, loop] { attempt(loop); });
            } else {
                loop->stop(); // gone, and no reconnecting: run() returns
            }
        });
        loop->run([this, loop] { attempt(loop); });
        std::optional<std::string> failed;
        {
            const std::lock_guard lock{mutex_};
            running_ = false;
            ended_ = true;
            connection_.reset();
            failed = failure_;
        }
        changed_.notify_all();
        if (failed && !stop_asked_) throw std::runtime_error("sc-tcp: " + *failed);
    }

    void stop() {
        stop_asked_ = true;
        if (const auto loop = current()) loop->stop();
    }

    bool wait_until_connected(const std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        changed_.wait_for(lock, timeout, [this] { return connection_ != nullptr || ended_; });
        return connection_ != nullptr;
    }

    std::shared_ptr<tcp_connection> connection() const {
        const std::lock_guard lock{mutex_};
        return connection_;
    }

    tcp_stats stats() const {
        const auto loop = current();
        return loop ? loop->stats() : tcp_stats{};
    }

private:
    // A connection attempt, on the I/O thread.
    void attempt(const std::shared_ptr<tcp::event_loop> &loop) {
        loop->connect(server_, options_.connect_timeout,
                      [this](const std::shared_ptr<tcp_connection> &c) {
                          delay_ = options_.reconnect; // connected: back to the first delay
                          {
                              const std::lock_guard lock{mutex_};
                              connection_ = c;
                          }
                          changed_.notify_all();
                      },
                      [this, loop](const std::string &why) {
                          if (options_.reconnect.count() <= 0) {
                              {
                                  const std::lock_guard lock{mutex_};
                                  failure_ = why;
                              }
                              loop->stop();
                              return;
                          }
                          tcp::warn("connecting to " + server_.to_string() + " failed (" + why + "), again in " +
                                           std::to_string(delay_.count()) + " ms");
                          const auto wait = delay_;
                          delay_ = std::min(delay_ * 2, std::max(options_.max_reconnect, options_.reconnect));
                          loop->after(wait, [this, loop] { attempt(loop); });
                      });
    }

    std::shared_ptr<tcp::event_loop> current() const {
        const std::lock_guard lock{mutex_};
        return loop_;
    }

    ip_endpoint server_;
    tcp_handlers handlers_;
    tcp_client_options options_;
    std::chrono::milliseconds delay_; // the I/O thread's
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool running_ = false, ended_ = false;
    std::atomic<bool> stop_asked_{false};
    std::optional<std::string> failure_;
    std::shared_ptr<tcp_connection> connection_;
    std::shared_ptr<tcp::event_loop> loop_;
};

sc::tcp_client::tcp_client(ip_endpoint server, tcp_handlers handlers, tcp_client_options options)
    : implementation_(std::make_unique<implementation>(std::move(server), std::move(handlers), options)) {
}

sc::tcp_client::~tcp_client() = default;
void sc::tcp_client::run() { implementation_->run(); }
void sc::tcp_client::stop() { implementation_->stop(); }
bool sc::tcp_client::wait_until_connected(const std::chrono::milliseconds timeout) { return implementation_->wait_until_connected(timeout); }
std::shared_ptr<sc::tcp_connection> sc::tcp_client::connection() const { return implementation_->connection(); }
bool sc::tcp_client::send(const std::string_view bytes) {
    const auto c = implementation_->connection();
    return c && c->send(bytes);
}
sc::tcp_stats sc::tcp_client::stats() const { return implementation_->stats(); }
void sc::tcp_client::log_level(const sc::log_level level) { tcp_log_level(level); }
