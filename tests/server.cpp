// The server on loopback with real sockets: framing over partial and joined reads, order and
// counts with many clients at once, several ports, idle timeouts and ticks, sending from other
// threads, close() after output, handlers that throw, input and output limits, the connection
// limit, stopping with connections open, and binding errors.

#include <tcp.h>
#include <sc_test.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
    // A blocking test client.
    class client {
    public:
        explicit client(const int port) {
            fd_ = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<std::uint16_t>(port));
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            if (connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) throw std::runtime_error("connect failed");
        }
        ~client() {
            if (fd_ >= 0) ::close(fd_);
        }
        void write(const std::string &bytes) const {
            std::size_t done = 0;
            while (done < bytes.size()) {
                const auto n = ::send(fd_, bytes.data() + done, bytes.size() - done, 0);
                if (n <= 0) return;
                done += static_cast<std::size_t>(n);
            }
        }
        // Up to count bytes within timeout; fewer at EOF.
        std::string read(const std::size_t count, const std::chrono::milliseconds timeout = 3s) const {
            std::string out;
            const auto until = std::chrono::steady_clock::now() + timeout;
            while (out.size() < count) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
                if (left.count() <= 0) break;
                pollfd ready{fd_, POLLIN, 0};
                if (poll(&ready, 1, static_cast<int>(left.count())) <= 0) break;
                char buffer[65536];
                const auto n = ::recv(fd_, buffer, std::min(sizeof buffer, count - out.size()), 0);
                if (n <= 0) break;
                out.append(buffer, static_cast<std::size_t>(n));
            }
            return out;
        }
        // Whether the server closed it (EOF) within timeout.
        bool closed_by_server(const std::chrono::milliseconds timeout = 3s) const {
            const auto until = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < until) {
                pollfd ready{fd_, POLLIN, 0};
                if (poll(&ready, 1, 100) > 0) {
                    char buffer[4096];
                    const auto n = ::recv(fd_, buffer, sizeof buffer, 0);
                    if (n <= 0) return true;
                }
            }
            return false;
        }
        void close_now() {
            ::close(fd_);
            fd_ = -1;
        }

    private:
        int fd_ = -1;
    };

    // "<n>:<n bytes>" frames: received answers each with "ok:<payload>\n".
    std::size_t framed(sc::tcp_connection &c, const std::string_view input) {
        const auto colon = input.find(':');
        if (colon == std::string_view::npos) return 0;
        const auto length = std::stoul(std::string{input.substr(0, colon)});
        if (input.size() < colon + 1 + length) return 0;
        c.send("ok:" + std::string{input.substr(colon + 1, length)} + "\n");
        return colon + 1 + length;
    }

    template<typename Predicate>
    bool eventually(Predicate predicate, const std::chrono::milliseconds timeout = 3s) {
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < until) {
            if (predicate()) return true;
            std::this_thread::sleep_for(10ms);
        }
        return predicate();
    }

    const sc::ip_endpoints any_port{{"127.0.0.1", 0}}; // the system picks a free port

    struct running {
        sc::tcp_server &server;
        std::thread thread;
        explicit running(sc::tcp_server &s) : server(s), thread([&s] { s.run(); }) {
            if (!s.wait_until_running()) throw std::runtime_error("server didn't start");
        }
        ~running() {
            server.stop();
            thread.join();
        }
        int port(const std::size_t i = 0) const { return server.listening().at(i).port; }
    };
}

int main() {
    using std::string;

    SECTION("Frames over partial and joined reads; connected first, disconnected last, with the reason");
    {
        std::mutex m;
        std::vector<string> seen;
        sc::tcp_server server{any_port, {
            .connected = [&](sc::tcp_connection &c) { const std::lock_guard l{m}; seen.push_back("connected " + c.remote().host); },
            .received = framed,
            .disconnected = [&](sc::tcp_connection &, const sc::tcp_close_reason reason) {
                const std::lock_guard l{m};
                seen.push_back("disconnected " + string{sc::tcp_close_reason_name(reason)});
            },
        }};
        running r{server};
        {
            client c{r.port()};
            for (const char ch: string{"5:hello"}) { // a byte at a time
                c.write(string(1, ch));
                std::this_thread::sleep_for(5ms);
            }
            CHECK_EQ(c.read(9), string{"ok:hello\n"});
            c.write("3:abc2:de1:f"); // three in one write
            CHECK_EQ(c.read(20), string{"ok:abc\nok:de\nok:f\n"});
            c.write("4:ab"); // half of one: the rest waits
            CHECK_EQ(c.read(1, 200ms), string{});
            c.write("cd");
            CHECK_EQ(c.read(8), string{"ok:abcd\n"});
        }
        CHECK(eventually([&] { const std::lock_guard l{m}; return seen.size() == 2; }));
        const std::lock_guard l{m};
        CHECK_EQ(seen.front(), string{"connected 127.0.0.1"});
        CHECK_EQ(seen.back(), string{"disconnected peer"});
        CHECK_EQ(server.stats().accepted, std::uint64_t{1});
    }

    SECTION("Many clients at once: each one's frames answered, in order");
    {
        sc::tcp_server server{any_port, {.received = framed}, {.threads = 4}};
        running r{server};
        constexpr int clients = 100, frames = 50;
        std::atomic<int> good{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < clients; ++i) {
            threads.emplace_back([&, i] {
                client c{r.port()};
                string expected, sent;
                for (int f = 0; f < frames; ++f) {
                    const auto payload = std::to_string(i) + "-" + std::to_string(f);
                    sent += std::to_string(payload.size()) + ":" + payload;
                    expected += "ok:" + payload + "\n";
                }
                c.write(sent);
                if (c.read(expected.size(), 10s) == expected) ++good;
            });
        }
        for (auto &t: threads) t.join();
        CHECK_EQ(good.load(), clients);
        CHECK(eventually([&] { return server.stats().connections == 0; }));
        CHECK_EQ(server.stats().accepted, std::uint64_t{clients});
    }

    SECTION("Several ports: local() says which");
    {
        std::mutex m;
        std::set<int> ports;
        sc::tcp_server server{sc::ip_endpoints{{"127.0.0.1", 0}, {"127.0.0.1", 0}}, {.connected = [&](sc::tcp_connection &c) {
            const std::lock_guard l{m};
            ports.insert(c.local().port);
        }}};
        running r{server};
        CHECK_EQ(server.listening().size(), std::size_t{2});
        client a{r.port(0)}, b{r.port(1)};
        CHECK(eventually([&] { const std::lock_guard l{m}; return ports.size() == 2; }));
        const std::lock_guard l{m};
        CHECK(ports.contains(r.port(0)) && ports.contains(r.port(1)));
    }

    SECTION("Idle connections closed, ticks between");
    {
        std::atomic<int> ticks{0};
        std::atomic<int> idle{0};
        sc::tcp_server server{any_port, {
            .tick = [&](sc::tcp_connection &) { ++ticks; },
            .disconnected = [&](sc::tcp_connection &, const sc::tcp_close_reason reason) {
                if (reason == sc::tcp_close_reason::idle) ++idle;
            },
        }, {.idle_timeout = 700ms, .tick = 100ms}};
        running r{server};
        client c{r.port()};
        CHECK(c.closed_by_server(3s));
        CHECK(eventually([&] { return idle.load() == 1; }));
        CHECK(ticks.load() >= 3);
    }

    SECTION("Sending from another thread; close() delivers what was sent first");
    {
        std::atomic<int> closed{0};
        sc::tcp_server server{any_port, {.disconnected = [&](sc::tcp_connection &, const sc::tcp_close_reason reason) {
            if (reason == sc::tcp_close_reason::closed) ++closed;
        }}};
        running r{server};
        client c{r.port()};
        CHECK(eventually([&] { return server.connections().size() == 1; }));
        const auto connection = server.connections().front();
        std::thread([connection] {
            connection->send("from elsewhere\n");
            connection->close();
        }).join();
        CHECK_EQ(c.read(15), string{"from elsewhere\n"});
        CHECK(c.closed_by_server());
        CHECK(eventually([&] { return closed.load() == 1; }));
        CHECK(!connection->open());
        CHECK(!connection->send("too late")); // a kept connection fails cleanly
    }

    SECTION("A handler that throws closes its connection, not the server");
    {
        std::atomic<int> errors{0};
        sc::tcp_server server{any_port, {
            .received = [](sc::tcp_connection &c, const std::string_view input) -> std::size_t {
                if (input.starts_with("boom")) throw std::runtime_error("bad frame");
                return framed(c, input);
            },
            .disconnected = [&](sc::tcp_connection &, const sc::tcp_close_reason reason) {
                if (reason == sc::tcp_close_reason::error) ++errors;
            },
        }};
        sc::tcp_server::log_level(sc::log_level::error); // the expected warning, not shown
        running r{server};
        client bad{r.port()}, good{r.port()};
        bad.write("boom");
        CHECK(bad.closed_by_server());
        good.write("2:hi");
        CHECK_EQ(good.read(6), string{"ok:hi\n"});
        CHECK(eventually([&] { return errors.load() == 1; }));
        sc::tcp_server::log_level(sc::log_level::warning);
    }

    SECTION("Input past max_input, output past max_output: closed");
    {
        std::atomic<int> in{0}, out{0};
        sc::tcp_server server{any_port, {
            .received = [](sc::tcp_connection &c, const std::string_view input) -> std::size_t {
                if (input.starts_with("flood")) c.send(string(1 << 20, 'x')); // the client won't read
                return 0;                                                     // never consumes
            },
            .disconnected = [&](sc::tcp_connection &, const sc::tcp_close_reason reason) {
                if (reason == sc::tcp_close_reason::input_overflow) ++in;
                if (reason == sc::tcp_close_reason::output_overflow) ++out;
            },
        }, {.max_input = 64 * 1024, .max_output = 256 * 1024}};
        running r{server};
        {
            client c{r.port()};
            c.write(string(100 * 1024, 'a'));
            CHECK(c.closed_by_server());
        }
        CHECK(eventually([&] { return in.load() == 1; }));
        {
            client c{r.port()};
            for (int i = 0; i < 40 && out.load() == 0; ++i) { // each asks for a MB more than it reads
                c.write("flood");
                std::this_thread::sleep_for(20ms);
            }
            CHECK(eventually([&] { return out.load() == 1; }));
        }
    }

    SECTION("Past max_connections: refused");
    {
        sc::tcp_server server{any_port, {}, {.max_connections = 2}};
        sc::tcp_server::log_level(sc::log_level::error);
        running r{server};
        client a{r.port()}, b{r.port()};
        CHECK(eventually([&] { return server.stats().connections == 2; }));
        client c{r.port()};
        CHECK(c.closed_by_server());
        CHECK_EQ(server.stats().refused, std::uint64_t{1});
        sc::tcp_server::log_level(sc::log_level::warning);
    }

    SECTION("stop() with connections open: each disconnected, run() returns");
    {
        std::atomic<int> stopped{0};
        sc::tcp_server server{any_port, {.disconnected = [&](sc::tcp_connection &, const sc::tcp_close_reason reason) {
            if (reason == sc::tcp_close_reason::stopped) ++stopped;
        }}};
        std::thread serving{[&] { server.run(); }};
        CHECK(server.wait_until_running());
        const int port = server.listening().front().port;
        client a{port}, b{port}, c{port};
        CHECK(eventually([&] { return server.stats().connections == 3; }));
        server.stop();
        serving.join();
        CHECK_EQ(stopped.load(), 3);
        CHECK(a.closed_by_server());
    }

    SECTION("A port already taken: run() throws, wait_until_running() says so");
    {
        sc::tcp_server first{any_port, {}};
        running r{first};
        sc::tcp_server second{"127.0.0.1:" + std::to_string(r.port()), {}};
        std::atomic<bool> threw{false};
        std::thread t{[&] {
            try {
                second.run();
            } catch (const std::runtime_error &) {
                threw = true;
            }
        }};
        CHECK(!second.wait_until_running(2s));
        t.join();
        CHECK(threw.load());
        CHECK_THROWS_AS(sc::tcp_server(sc::ip_endpoints{}, {}), std::invalid_argument);
        CHECK_EQ(string{sc::tcp_close_reason_name(sc::tcp_close_reason::output_overflow)}, string{"output_overflow"});
    }

    TEST_SUMMARY();
}
