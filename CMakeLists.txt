cmake_minimum_required(VERSION 3.22)
set(SC_MODULE tcp-server)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-tcp-server" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_TCP_SERVER_BUILD_TESTS "Build simply-cpp-tcp-server tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc-core: ip_endpoints, log_level and params for the demo. No other dependency: epoll (Linux) or
# kqueue (macOS) and POSIX sockets.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.20.0)
find_package(Threads REQUIRED)

set(SOURCE_OBJECTS include/tcp_server.h)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(tcp_server
        PUBLIC_LINK_LIBRARIES sc::sc-core Threads::Threads)

add_sc_libraries(DESCRIPTION "simply-cpp TCP server")
target_compile_features(sc-tcp-server PUBLIC cxx_std_20)
target_compile_features(sc-tcp-server-shared PUBLIC cxx_std_20)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-tcp-server
        COMPONENT development)

if (SC_TCP_SERVER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-tcp-server-test)
endif ()

add_sc_example(sc-tcp-server-demo)

package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 1.20.0~)")
