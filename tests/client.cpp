// sc::tcp_client against an sc::tcp_server on loopback: connecting and talking, reconnecting when
// the server goes and comes back, failing cleanly without one, sending while not connected, ticks
// and the idle timeout, stopping.

#include <tcp.h>
#include <sc_test.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
    template<typename Predicate>
    bool eventually(Predicate predicate, const std::chrono::milliseconds timeout = 5s) {
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < until) {
            if (predicate()) return true;
            std::this_thread::sleep_for(10ms);
        }
        return predicate();
    }

    // An echo server: each line back as "echo:<line>".
    std::size_t echo(sc::tcp_connection &c, const std::string_view input) {
        const auto end = input.find('\n');
        if (end == std::string_view::npos) return 0;
        c.send("echo:" + std::string{input.substr(0, end)} + "\n");
        return end + 1;
    }

    // A server on its own thread, on a given port (0: a free one).
    struct echo_server {
        sc::tcp_server server;
        std::thread thread;
        explicit echo_server(const int port = 0) : server{sc::ip_endpoints{{"127.0.0.1", port}}, {.received = echo}} {
            thread = std::thread{[this] { server.run(); }};
            if (!server.wait_until_running()) throw std::runtime_error("server didn't start");
        }
        ~echo_server() {
            server.stop();
            thread.join();
        }
        int port() const { return server.listening().front().port; }
    };

    // Lines a client received.
    struct lines {
        std::mutex mutex;
        std::vector<std::string> seen;
        std::size_t operator()(sc::tcp_connection &, const std::string_view input) {
            const auto end = input.find('\n');
            if (end == std::string_view::npos) return 0;
            const std::lock_guard lock{mutex};
            seen.emplace_back(input.substr(0, end));
            return end + 1;
        }
        std::size_t count() {
            const std::lock_guard lock{mutex};
            return seen.size();
        }
    };
}

int main() {
    using std::string;

    SECTION("Connects, talks, and is told about it in order");
    {
        echo_server s;
        lines got;
        std::mutex m;
        std::vector<string> events;
        sc::tcp_client client{{"127.0.0.1", s.port()}, {
            .connected = [&](sc::tcp_connection &c) {
                const std::lock_guard l{m};
                events.push_back("connected to " + std::to_string(c.remote().port));
            },
            .received = [&](sc::tcp_connection &c, const std::string_view input) { return got(c, input); },
            .disconnected = [&](sc::tcp_connection &, const sc::tcp_close_reason reason) {
                const std::lock_guard l{m};
                events.push_back("disconnected " + string{sc::tcp_close_reason_name(reason)});
            },
        }};
        std::thread t{[&] { client.run(); }};
        CHECK(client.wait_until_connected());
        CHECK(client.connection() != nullptr);
        CHECK_EQ(client.connection()->remote().port, s.port());
        CHECK(client.send("one\ntwo\n"));
        CHECK(eventually([&] { return got.count() == 2; }));
        {
            const std::lock_guard l{got.mutex};
            CHECK_EQ(got.seen.at(0), string{"echo:one"});
            CHECK_EQ(got.seen.at(1), string{"echo:two"});
        }
        CHECK(client.stats().bytes_received >= 16);
        client.stop();
        t.join();
        CHECK(!client.connection());
        CHECK(!client.send("after")); // not connected: nothing sent
        const std::lock_guard l{m};
        CHECK_EQ(events.size(), std::size_t{2});
        CHECK_EQ(events.front(), "connected to " + std::to_string(s.port()));
        CHECK_EQ(events.back(), string{"disconnected stopped"});
    }

    SECTION("Reconnects when the server goes and comes back");
    {
        std::atomic<int> connects{0}, drops{0};
        int port = 0;
        auto first = std::make_unique<echo_server>();
        port = first->port();
        sc::tcp_client::log_level(sc::log_level::error); // the retries' warnings, not shown
        sc::tcp_client client{{"127.0.0.1", port}, {
            .connected = [&](sc::tcp_connection &) { ++connects; },
            .disconnected = [&](sc::tcp_connection &, sc::tcp_close_reason) { ++drops; },
        }, {.reconnect = 50ms, .max_reconnect = 200ms}};
        std::thread t{[&] { client.run(); }};
        CHECK(client.wait_until_connected());
        first.reset(); // the server goes: the client loses its connection, then retries
        CHECK(eventually([&] { return drops.load() == 1; }));
        CHECK(!client.wait_until_connected(300ms));
        echo_server second{port}; // it comes back on the same port
        CHECK(eventually([&] { return connects.load() == 2; }));
        CHECK(client.wait_until_connected());
        CHECK(client.stats().refused >= 1); // the attempts while it was gone
        client.stop();
        t.join();
        sc::tcp_client::log_level(sc::log_level::warning);
    }

    SECTION("Without reconnect: no server is an error from run(), saying where");
    {
        int port = 0;
        { echo_server gone; port = gone.port(); } // a port nothing listens on any more
        sc::tcp_client client{{"127.0.0.1", port}, {}, {.connect_timeout = 1s}};
        std::optional<string> error;
        std::thread t{[&] {
            try {
                client.run();
            } catch (const std::runtime_error &e) {
                error = e.what();
            }
        }};
        CHECK(!client.wait_until_connected(3s));
        t.join();
        CHECK(error.has_value());
        if (error) CHECK(error->find("127.0.0.1:" + std::to_string(port)) != string::npos);

        sc::tcp_client nowhere{{"nowhere.invalid", 80}, {}};
        CHECK_THROWS_AS(nowhere.run(), std::runtime_error);
        CHECK_THROWS_AS((sc::tcp_client{{"", 80}, {}}), std::invalid_argument);
    }

    SECTION("Without reconnect: run() returns once the connection is gone");
    {
        auto s = std::make_unique<echo_server>();
        std::atomic<bool> returned{false};
        sc::tcp_client client{{"127.0.0.1", s->port()}, {}};
        std::thread t{[&] {
            client.run();
            returned = true;
        }};
        CHECK(client.wait_until_connected());
        s.reset();
        CHECK(eventually([&] { return returned.load(); }));
        t.join();
    }

    SECTION("Ticks, and the idle timeout, on a client too");
    {
        echo_server s;
        std::atomic<int> ticks{0}, idle{0};
        sc::tcp_client client{{"127.0.0.1", s.port()}, {
            .tick = [&](sc::tcp_connection &) { ++ticks; },
            .disconnected = [&](sc::tcp_connection &, const sc::tcp_close_reason reason) {
                if (reason == sc::tcp_close_reason::idle) ++idle;
            },
        }, {.idle_timeout = 600ms, .tick = 100ms}};
        std::thread t{[&] { client.run(); }}; // no reconnect: returns after the idle close
        CHECK(client.wait_until_connected());
        t.join();
        CHECK_EQ(idle.load(), 1);
        CHECK(ticks.load() >= 3);
    }

    TEST_SUMMARY();
}
