# simply-cpp-tcp

`sc-tcp` gives TCP servers and clients for many long-lived connections: devices that connect and
keep talking, such as GPS trackers, and the clients that talk to such servers. Both share one
model: you write handlers for a connection (connected, bytes received, a tick, disconnected), and
they run one at a time and in order for each connection. One thread waits on every socket (epoll on
Linux, kqueue on macOS) and a few worker threads run the handlers, so thousands of connections cost
a few threads and little memory, not a thread each.

It was `sc-tcp-server` up to 1.0.2: the packages `simply-cpp-tcp(-dev)` replace
`simply-cpp-tcp-server(-dev)`. `tcp_server` and its handlers kept their names.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has a [TCP guide](https://github.com/roelofrossouw/simply-cpp/wiki/TCP) and the [sc-tcp reference](https://github.com/roelofrossouw/simply-cpp/wiki/Reference-sc-tcp), plus [getting started](https://github.com/roelofrossouw/simply-cpp/wiki/Getting-Started) for the whole suite.

```cpp
#include <tcp.h>

std::size_t received(sc::tcp_connection &c, std::string_view input) {
    if (input.size() < 4) return 0;      // not a whole frame yet: wait for more
    c.send("\x01");
    return 4;                            // consumed; the rest stays for next time
}

sc::tcp_server server{"0.0.0.0:5400;0.0.0.0:5500", {.received = received},
                      {.idle_timeout = std::chrono::minutes{15}, .tick = std::chrono::seconds{5}}};
std::thread serving{[&] { server.run(); }};     // until server.stop()

sc::tcp_client client{{"listener.example.com", 5400}, {.received = answered},
                      {.reconnect = std::chrono::seconds{5}}};
std::thread talking{[&] { client.run(); }};     // connects, and again when it drops
client.wait_until_connected();
client.send(frame);
```

## Handlers (`tcp_handlers.h`)

| Handler | When |
|---|---|
| `connected(c)` | first, once connected (accepted, for a server) |
| `received(c, input)` | bytes arrived. `input` is everything received and not yet consumed; return how many bytes from its start you consumed (0 waits for more). Called again while you consume and input is left, so one call per frame works |
| `tick(c)` | every `tick` (an option), between the others: to send queued commands, say |
| `disconnected(c, reason)` | last, once it's closed: `peer`, `closed`, `idle`, `error` (a socket error, or a handler threw), `input_overflow`, `output_overflow` or `stopped` |

A handler that throws closes its connection (`error`), not the server or client. `c.context()` is a
`std::any` for your per-connection state; only the handlers touch it.

## Connections (`tcp_connection.h`)

`sc::tcp_connection` gives `id()`, `remote()`, `local()` (which port it came in on), `connected_at()`
and `open()`, and two calls that work from any thread: `send(bytes)` (now, or queued for when the
socket takes it; false once closed, or past `max_output`, which closes it) and `close()` (after what
was sent is out, within 5 s). `server.connections()` lists a server's open ones; a client has
`connection()`.

## Servers (`tcp_server.h`)

| Option | Default | |
|---|---|---|
| `threads` | one a CPU, at least 2 | workers running handlers |
| `idle_timeout` | 0: never | close a connection that sent nothing this long |
| `tick` | 0: never | how often `tick` runs for each connection |
| `max_connections` | 10000 | more are accepted and closed at once (counted as refused) |
| `max_input`, `max_output` | 1 MB, 4 MB | unconsumed input and unsent output a connection may hold |
| `backlog` | 512 | connections waiting to be accepted |

`run()` throws when it can't bind; `wait_until_running()` says when it's accepting, and
`listening()` the ports bound (an `ip_endpoint` with port 0 gets a free one). `stop()`, from any
thread, closes every connection (`stopped`) and lets the handlers finish before `run()` returns.
`stats()` counts connections, those accepted and refused, and bytes.

## Clients (`tcp_client.h`)

| Option | Default | |
|---|---|---|
| `connect_timeout` | 10 s | per address tried (a name can resolve to several) |
| `reconnect` | 0: don't | after a failed attempt or a dropped connection, connect again after this, doubling up to `max_reconnect` (60 s) |
| `threads` | 1 | workers running the handlers |
| `idle_timeout`, `tick`, `max_input`, `max_output` | as for a server | |

`run()` connects and talks until `stop()`. Without `reconnect` it returns once the connection is
gone, and throws `std::runtime_error` (saying where and why) when it couldn't connect.
`wait_until_connected()`, `connection()` and `send()` (false while not connected) work from any
thread.

## The classes under them (namespace `sc::tcp`)

Each in a header of its own, for programs that want them directly:

| Header | Class | |
|---|---|---|
| `tcp_socket.h` | `sc::tcp::socket` | a socket file descriptor, owned: listening, accepting, a non-blocking connect, reads and writes. Addresses come from `sc::ip_endpoint` (sc-core) |
| `tcp_poller.h` | `sc::tcp::poller` | waiting on many sockets: epoll or kqueue behind one interface |
| `tcp_worker_pool.h` | `sc::tcp::worker_pool`, `sc::tcp::strand` | worker threads; a strand's tasks run one at a time, in order |
| `tcp_event_loop.h` | `sc::tcp::event_loop` | the I/O loop both front ends are built on: listeners, outgoing connects, timers, ticks, idle timeouts, closing |

`sc::tcp_log_level()` sets what sc-tcp writes to stderr (warnings by default).

## Demo

`sc-tcp-demo` is a line echo server (each line back upper-cased, numbered per connection), and with
`--connect host:port` a client for it that sends the lines you type and prints the answers.

<!-- sc-example: examples/sc-tcp-demo.cpp -->
```cpp
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
```
<!-- /sc-example -->

## Tests

`server` and `client` (label `unit`) run real sockets on loopback. The server's: framing over
partial and joined reads, order with 100 clients at once, several ports, idle timeouts and ticks,
sending from another thread, `close()` after output, handlers that throw, the input and output
limits, the connection limit, stopping, a port already taken. The client's: talking, reconnecting
when the server goes and comes back, failing cleanly without one, ticks and the idle timeout. At
production scale (6000 connections on one port, a 64-byte frame a second each for 30 s, on
build-noble) every one of the 180000 frames was answered, in 10 MB.

## Limits

- Plain TCP: no TLS (put a proxy in front for it).
- Handlers get bytes; framing (lengths, delimiters) is theirs.
- Idle timeouts, ticks and reconnect delays are checked about every 100 ms.
