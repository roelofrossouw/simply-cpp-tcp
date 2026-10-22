#pragma once

// sc-tcp: TCP servers and clients for long-lived connections (sc::tcp_server, sc::tcp_client),
// sharing one model: handlers on a connection (sc::tcp_connection) that run in order. The classes
// under them are in sc::tcp: tcp_socket.h, tcp_poller.h, tcp_worker_pool.h, tcp_event_loop.h.
#include "tcp_client.h"
#include "tcp_connection.h"
#include "tcp_handlers.h"
#include "tcp_server.h"
