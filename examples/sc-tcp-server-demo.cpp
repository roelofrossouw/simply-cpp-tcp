// A line echo server: each line a client sends comes back upper-cased, with the connection's
// count so far. Try it with `nc 127.0.0.1 7000`; Ctrl+C stops it.

#include <params.h>
#include <tcp_server.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace {
    sc::tcp_server *running = nullptr;
    void stop(int) {
        if (running) running->stop();
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-tcp-server-demo"},
        {"description", "A line echo server on sc-tcp-server: each line comes back upper-cased, numbered per connection."},
        {"options", {
            {"listen", {{"short", "l"}, {"value", "ENDPOINTS"}, {"default", "127.0.0.1:7000"},
                        {"help", "where to listen, host:port separated by ';'"}}},
            {"idle", {{"short", "i"}, {"type", "integer"}, {"default", 60}, {"value", "SECONDS"},
                      {"help", "close a connection silent this long (0: never)"}}}}}}};
    try {
        // [readme]
        sc::tcp_server server{args.as<std::string>("listen"), {
            .connected = [](sc::tcp_connection &c) {
                c.context() = 0; // lines so far
                std::cout << "+ " << c.remote() << " on " << c.local() << std::endl;
            },
            .received = [](sc::tcp_connection &c, const std::string_view input) -> std::size_t {
                const auto end = input.find('\n');
                if (end == std::string_view::npos) return 0; // not a whole line yet
                std::string line{input.substr(0, end)};
                std::ranges::transform(line, line.begin(), [](const unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                auto &count = std::any_cast<int &>(c.context());
                c.send(std::to_string(++count) + ": " + line + "\n");
                return end + 1;
            },
            .disconnected = [](sc::tcp_connection &c, const sc::tcp_close_reason reason) {
                std::cout << "- " << c.remote() << " (" << sc::tcp_close_reason_name(reason) << ")" << std::endl;
            },
        }, {.idle_timeout = std::chrono::seconds{args.as<int>("idle")}}};
        // [/readme]
        running = &server;
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::thread serving{[&server] { server.run(); }};
        if (server.wait_until_running()) {
            for (const auto &endpoint: server.listening()) std::cout << "Listening on " << endpoint << std::endl;
        }
        serving.join();
    } catch (const std::exception &error) {
        std::cerr << "sc-tcp-server-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
