# simply-cpp-tcp-server

`sc-tcp-server` is a TCP server for many long-lived connections: devices that connect and keep
talking, such as GPS trackers. One thread waits on every socket (epoll on Linux, kqueue on macOS)
and a few worker threads run your handlers, so thousands of connections cost a few threads and
little memory, not a thread each. A connection's handlers run one at a time and in order, never
two at once; different connections' run side by side. One server can listen on several ports.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has a [TCP server guide](https://github.com/roelofrossouw/simply-cpp/wiki/TCP-Server) and the [sc-tcp-server reference](https://github.com/roelofrossouw/simply-cpp/wiki/Reference-sc-tcp-server), plus [getting started](https://github.com/roelofrossouw/simply-cpp/wiki/Getting-Started) for the whole suite.

```cpp
#include <tcp_server.h>

sc::tcp_server server{"0.0.0.0:5400;0.0.0.0:5500", {
    .connected = [](sc::tcp_connection &c) { c.context() = std::string{}; },
    .received = [](sc::tcp_connection &c, std::string_view input) -> std::size_t {
        if (input.size() < 4) return 0;     // not a whole frame yet: wait for more
        c.send("\x01");
        return 4;                            // consumed; the rest stays for next time
    },
    .disconnected = [](sc::tcp_connection &c, sc::tcp_close_reason why) { ... },
}, {.idle_timeout = std::chrono::minutes{15}, .tick = std::chrono::seconds{5}}};
std::thread serving{[&] { server.run(); }};   // until server.stop()
```

## Handlers

| Handler | When |
|---|---|
| `connected(c)` | first, once accepted |
| `received(c, input)` | bytes arrived. `input` is everything received and not yet consumed; return how many bytes from its start you consumed (0 waits for more). It's called again while you consume and input is left, so one call per frame works |
| `tick(c)` | every `options.tick`, between the others: to send queued commands, say |
| `disconnected(c, reason)` | last, once it's closed: `peer`, `closed`, `idle`, `error` (a socket error, or a handler threw), `input_overflow`, `output_overflow` or `server_stopped` |

A handler that throws closes its connection (`error`), not the server. `c.context()` is a
`std::any` for your per-connection state (a parser, an IMEI); only the handlers touch it.

## Connections

`tcp_connection` gives `id()`, `remote()`, `local()` (which port it came in on), `connected_at()`
and `open()`, and two calls that work from any thread: `send(bytes)` (now, or queued for when the
socket takes it; false once closed, or past `max_output`, which closes it) and `close()` (after
what was sent is out, within 5 s). `server.connections()` lists the open ones, to send to one from
elsewhere; `server.stats()` counts connections, accepted, refused and bytes.

## Options

| Option | Default | |
|---|---|---|
| `threads` | one a CPU, at least 2 | workers running handlers |
| `idle_timeout` | 0: never | close a connection that sent nothing this long |
| `tick` | 0: never | how often `tick` runs for each connection |
| `max_connections` | 10000 | more are accepted and closed at once (counted as refused) |
| `max_input` | 1 MB | unconsumed input a connection may hold |
| `max_output` | 4 MB | unsent output a connection may hold |
| `backlog` | 512 | connections waiting to be accepted |

`run()` throws when it can't bind; `wait_until_running()` says when it's accepting, and
`listening()` the ports bound (an `ip_endpoint` with port 0 gets a free one). `stop()`, from any
thread, closes every connection (`server_stopped`) and lets the handlers finish before `run()`
returns. `sc::tcp_server::log_level()` sets what it writes to stderr (warnings by default).

## Demo

`sc-tcp-server-demo --listen 127.0.0.1:7000` echoes each line back upper-cased and numbered per
connection; try it with `nc 127.0.0.1 7000`.

<!-- sc-example: examples/sc-tcp-server-demo.cpp -->
```cpp
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
```
<!-- /sc-example -->

## Tests

`server` (label `unit`) runs real sockets on loopback: framing over partial and joined reads,
order with 100 clients at once, several ports, idle timeouts and ticks, sending from another
thread, `close()` after output, handlers that throw, the input and output limits, the connection
limit, stopping with connections open, and a port already taken. At production scale (6000
connections on one port, a 64-byte frame a second each for 30 s, on build-noble) every one of the
180000 frames was answered, in 10 MB.

## Limits

- Plain TCP: no TLS (put a proxy in front for it).
- Handlers get bytes; framing (lengths, delimiters) is theirs.
- Idle timeouts and ticks are checked about every 100 ms, so they're that precise.
