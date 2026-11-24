#pragma once

// sc-tcp: TCP servers and clients for long-lived connections (sc::tcp::server, sc::tcp::client),
// sharing one model: handlers on a connection (sc::tcp::connection) that run in order. The classes
// under them are in sc::tcp: tcp_socket.h, tcp_poller.h, tcp_worker_pool.h, tcp_event_loop.h.
#include <sc/tcp/client.h>
#include <sc/tcp/connection.h>
#include <sc/tcp/handlers.h>
#include <sc/tcp/server.h>
