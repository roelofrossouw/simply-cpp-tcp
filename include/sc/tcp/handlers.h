#pragma once

#include <sc/tcp/connection.h>

#include <sc/core/log_level.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace sc::tcp {
    // What a tcp::server or tcp::client does with its connections. Any may be left empty. A
    // connection's handlers run one at a time and in this order: connected, then received and tick
    // as they come, disconnected last; different connections' run side by side.
    struct handlers {
        // First, once connected (accepted, for a server).
        std::function<void(tcp::connection &)> connected;
        // When bytes arrive: input is everything received and not yet consumed. Returns how many
        // bytes from the start it consumed; the rest stays, and more is added to it. Returning 0
        // waits for more. Called again while it consumes and input is left. What arrived before
        // the other side closed is still handed over (then disconnected runs, with peer); after
        // close() or any other reason, it isn't.
        std::function<std::size_t(tcp::connection &, std::string_view input)> received;
        // Every tick (the options'), between the others.
        std::function<void(tcp::connection &)> tick;
        // Last, once it's closed: nothing more can be sent.
        std::function<void(tcp::connection &, tcp::close_reason)> disconnected;
    };

    // Counts since the start.
    struct stats {
        std::size_t connections = 0;   // open now
        std::uint64_t accepted = 0;    // connections made (accepted by a server, connected by a client)
        std::uint64_t refused = 0;     // a server's over max_connections; a client's failed attempts
        std::uint64_t bytes_received = 0;
        std::uint64_t bytes_sent = 0;
    };

    // What sc-tcp writes to stderr (a handler that threw, a refused connection): warnings and up
    // by default, for every server and client in the process.
    void log_level(sc::log_level level);
}
