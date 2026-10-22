cmake_minimum_required(VERSION 3.22)
set(SC_MODULE tcp)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-tcp" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_TCP_BUILD_TESTS "Build simply-cpp-tcp tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc-core: ip_endpoint (it resolves and converts socket addresses), log_level, and params for the
# demo. No other dependency: epoll (Linux) or kqueue (macOS) and POSIX sockets.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.21.0)
find_package(Threads REQUIRED)

set(SOURCE_OBJECTS include/tcp.h)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

# The front ends (tcp_server, tcp_client, tcp_connection) and the classes under them, in sc::tcp:
# the socket, the poller (epoll or kqueue), the worker pool with its strands, and the event loop.
add_sc_object(tcp_socket PUBLIC_LINK_LIBRARIES sc::sc-core Threads::Threads)
add_sc_object(tcp_poller)
add_sc_object(tcp_worker_pool)
add_sc_object(tcp_event_loop SOURCES include/tcp_handlers.h)
add_sc_object(tcp_connection SOURCES include/tcp_connection_state.h)
add_sc_object(tcp_server)
add_sc_object(tcp_client)

add_sc_libraries(DESCRIPTION "simply-cpp TCP servers and clients")
target_compile_features(sc-tcp PUBLIC cxx_std_20)
target_compile_features(sc-tcp-shared PUBLIC cxx_std_20)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-tcp
        COMPONENT development)

if (SC_TCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-tcp-test)
endif ()

add_sc_example(sc-tcp-demo)

# It was simply-cpp-tcp-server up to 1.0.2: these packages take its place (and its files).
package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 1.21.0~)"
        BREAKS "simply-cpp-tcp-server (<< 2.0.0~)" REPLACES "simply-cpp-tcp-server (<< 2.0.0~)"
        DEV_BREAKS "simply-cpp-tcp-server-dev (<< 2.0.0~)" DEV_REPLACES "simply-cpp-tcp-server-dev (<< 2.0.0~)")
