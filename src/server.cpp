#include "tcp_event_loop.h"

#include <sc/tcp/server.h>

#include <condition_variable>
#include <mutex>
#include <stdexcept>

class sc::tcp::server::implementation {
public:
    implementation(ip_endpoints listen, tcp::handlers handlers, const tcp::server_options &options)
        : endpoints_(std::move(listen)), handlers_(std::move(handlers)), options_(options) {
        if (endpoints_.empty()) throw std::invalid_argument("sc-tcp: nothing to listen on");
    }

    ~implementation() { stop(); }

    void run() {
        {
            const std::lock_guard lock{mutex_};
            if (running_) throw std::runtime_error("sc-tcp: already running");
            running_ = true;
            failed_ = false;
            loop_ = std::make_shared<tcp::event_loop>(
                handlers_, tcp::loop_options{options_.threads, options_.idle_timeout, options_.max_connections,
                                                    {options_.max_input, options_.max_output, options_.tick}});
        }
        const auto loop = current();
        try {
            for (const auto &endpoint: endpoints_) loop->listen(tcp::socket::listen_on(endpoint, options_.backlog));
        } catch (...) {
            {
                const std::lock_guard lock{mutex_};
                running_ = false;
                failed_ = true;
            }
            changed_.notify_all();
            throw;
        }
        loop->run([this] {
            {
                const std::lock_guard lock{mutex_};
                accepting_ = true;
            }
            changed_.notify_all();
        });
        {
            const std::lock_guard lock{mutex_};
            accepting_ = false;
            running_ = false;
        }
        changed_.notify_all();
    }

    void stop() {
        if (const auto loop = current()) loop->stop();
    }

    bool wait_until_running(const std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        changed_.wait_for(lock, timeout, [this] { return accepting_ || failed_; });
        return accepting_;
    }

    std::vector<ip_endpoint> listening() const {
        const auto loop = current();
        return loop ? loop->listening() : std::vector<ip_endpoint>{};
    }

    tcp::stats stats() const {
        const auto loop = current();
        return loop ? loop->stats() : tcp::stats{};
    }

    std::vector<std::shared_ptr<tcp::connection>> connections() const {
        const auto loop = current();
        return loop ? loop->connections() : std::vector<std::shared_ptr<tcp::connection>>{};
    }

private:
    std::shared_ptr<tcp::event_loop> current() const {
        const std::lock_guard lock{mutex_};
        return loop_;
    }

    ip_endpoints endpoints_;
    tcp::handlers handlers_;
    tcp::server_options options_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool running_ = false, accepting_ = false, failed_ = false;
    std::shared_ptr<tcp::event_loop> loop_;
};

sc::tcp::server::server(ip_endpoints listen, tcp::handlers handlers, tcp::server_options options)
    : implementation_(std::make_unique<implementation>(std::move(listen), std::move(handlers), options)) {
}

sc::tcp::server::~server() = default;
void sc::tcp::server::run() { implementation_->run(); }
void sc::tcp::server::stop() { implementation_->stop(); }
bool sc::tcp::server::wait_until_running(const std::chrono::milliseconds timeout) { return implementation_->wait_until_running(timeout); }
std::vector<sc::ip_endpoint> sc::tcp::server::listening() const { return implementation_->listening(); }
sc::tcp::stats sc::tcp::server::stats() const { return implementation_->stats(); }
std::vector<std::shared_ptr<sc::tcp::connection>> sc::tcp::server::connections() const { return implementation_->connections(); }
void sc::tcp::server::log_level(const sc::log_level level) { tcp::log_level(level); }
