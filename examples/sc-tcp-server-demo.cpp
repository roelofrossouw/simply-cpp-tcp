// A line echo server: each line a client sends comes back upper-cased, with the connection's
// count so far. Try it with `nc 127.0.0.1 7000`; Ctrl+C stops it.

#include <params.h>
#include <tcp_server.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

// [readme]
namespace {
    std::string upper_cased(std::string text) {
        std::ranges::transform(text, text.begin(), [](const unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return text;
    }

    // A client connected: its count of lines starts at 0.
    void connected(sc::tcp_connection &client) {
        client.context() = 0;
        std::cout << "+ " << client.remote() << " on " << client.local() << std::endl;
    }

    // Bytes arrived: answers the first whole line, and says how much of the input that used. The
    // server calls it again while lines are left.
    std::size_t received(sc::tcp_connection &client, const std::string_view input) {
        const auto end = input.find('\n');
        if (end == std::string_view::npos) return 0; // not a whole line yet: wait for more
        auto &count = std::any_cast<int &>(client.context());
        client.send(std::to_string(++count) + ": " + upper_cased(std::string{input.substr(0, end)}) + "\n");
        return end + 1;
    }

    // The client is gone, and why.
    void disconnected(sc::tcp_connection &client, const sc::tcp_close_reason reason) {
        std::cout << "- " << client.remote() << " (" << sc::tcp_close_reason_name(reason) << ")" << std::endl;
    }

    // The server: where it listens, the handlers above, and when a silent client is let go.
    sc::tcp_server line_server(const sc::ip_endpoints &listen, const std::chrono::seconds idle) {
        return {listen, {.connected = connected, .received = received, .disconnected = disconnected}, {.idle_timeout = idle}};
    }
}
// [/readme]

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
        auto server = line_server(args.as<std::string>("listen"), std::chrono::seconds{args.as<int>("idle")});
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
