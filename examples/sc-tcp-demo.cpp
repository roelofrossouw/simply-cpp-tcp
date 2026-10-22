// sc-tcp's two halves. By default a line echo server: each line a client sends comes back
// upper-cased, numbered per connection (try it with `nc 127.0.0.1 7000`). With --connect, a client
// for it: the lines typed go to the server, and its answers are printed. Ctrl+C stops either.
//
//     sc-tcp-demo                               # the server, on 127.0.0.1:7000
//     sc-tcp-demo --connect 127.0.0.1:7000      # a client, in another terminal

#include <params.h>
#include <tcp.h>

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

    // --- the server ---

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

    // --- the client ---

    // An answer arrived: prints each whole line.
    std::size_t answered(sc::tcp_connection &, const std::string_view input) {
        const auto end = input.find('\n');
        if (end == std::string_view::npos) return 0;
        std::cout << "< " << input.substr(0, end) << std::endl;
        return end + 1;
    }

    // The connection is gone: unless the client was stopped, it connects again.
    void lost(sc::tcp_connection &server, const sc::tcp_close_reason reason) {
        if (reason == sc::tcp_close_reason::stopped) return;
        std::cout << "(" << server.remote() << " gone: " << sc::tcp_close_reason_name(reason) << "; connecting again)" << std::endl;
    }

    // The client: connects to the server, and again 2 s after it's lost (doubling to 30 s).
    sc::tcp_client line_client(const sc::ip_endpoint &server) {
        return {server, {.received = answered, .disconnected = lost},
                {.reconnect = std::chrono::seconds{2}, .max_reconnect = std::chrono::seconds{30}}};
    }
}
// [/readme]

namespace {
    sc::tcp_server *serving = nullptr;
    sc::tcp_client *talking = nullptr;

    void stop(int) {
        if (serving) serving->stop();
        if (talking) talking->stop();
    }

    int serve(const std::string &listen, const std::chrono::seconds idle) {
        auto server = line_server(listen, idle);
        serving = &server;
        std::thread running{[&server] { server.run(); }};
        if (server.wait_until_running()) {
            for (const auto &endpoint: server.listening()) std::cout << "Listening on " << endpoint << std::endl;
        }
        running.join();
        return 0;
    }

    int talk(const std::string &to) {
        auto client = line_client(sc::ip_endpoint::parse(to));
        talking = &client;
        std::thread running{[&client] { client.run(); }};
        if (client.wait_until_connected()) std::cout << "Connected to " << client.connection()->remote() << "; type lines" << std::endl;
        for (std::string line; std::getline(std::cin, line);) {
            if (!client.send(line + "\n")) std::cout << "(not connected: not sent)" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{300}); // the last answers
        client.stop();
        running.join();
        return 0;
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-tcp-demo"},
        {"description", "sc-tcp's server and client: a line echo server (each line back upper-cased and numbered per "
                        "connection), or with --connect a client that sends the lines typed and prints the answers."},
        {"options", {
            {"listen", {{"short", "l"}, {"value", "ENDPOINTS"}, {"default", "127.0.0.1:7000"},
                        {"help", "the server: where to listen, host:port separated by ';'"}}},
            {"idle", {{"short", "i"}, {"type", "integer"}, {"default", 60}, {"value", "SECONDS"},
                      {"help", "the server: close a connection silent this long (0: never)"}}},
            {"connect", {{"short", "c"}, {"value", "HOST:PORT"}, {"default", ""},
                         {"help", "be a client of this server instead"}}}}}}};
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        if (const auto to = args.as<std::string>("connect"); !to.empty()) return talk(to);
        return serve(args.as<std::string>("listen"), std::chrono::seconds{args.as<int>("idle")});
    } catch (const std::exception &error) {
        std::cerr << "sc-tcp-demo: " << error.what() << '\n';
        return 1;
    }
}
