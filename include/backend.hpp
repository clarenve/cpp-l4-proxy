#pragma once

#include "socket.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace l4{

    struct Backend{
        std::string address;
        std::uint16_t port;
        std::atomic<bool> healthy{true};

        Backend(
            std::string address,
            std::uint16_t port
        );
    };


    struct BackendConnection{
        Backend* backend;
        Socket socket;
    };


    void set_backend_health(
        Backend& backend,
        bool is_healthy
    );

    BackendConnection
    connect_to_healthy_backend();

    void health_check_loop();

}