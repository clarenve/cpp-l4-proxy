#include "backend.hpp"

#include "errors.hpp"
#include "shutdown.hpp"
#include "socket.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <utility>

namespace l4{

    namespace{

        constexpr auto backend_connect_timeout =
            std::chrono::milliseconds{1000};

        constexpr auto health_check_timeout =
            std::chrono::milliseconds{500};


        std::array<Backend, 3> backends{
            Backend{"127.0.0.1", 9001},
            Backend{"127.0.0.1", 9002},
            Backend{"127.0.0.1", 9003}
        };


        std::atomic<std::size_t>
            next_backend_index{0};


        enum class HealthCheckResult{
            healthy,
            unhealthy,
            check_failed
        };


        std::optional<Socket>
        try_connect_to_backend(
            const Backend& backend,
            std::chrono::milliseconds timeout
        ){
            const int socket_fd =
                ::socket(
                    AF_INET,
                    SOCK_STREAM,
                    0
                );

            if(socket_fd == -1){
                throw_system_error(
                    "socket",
                    errno
                );
            }

            Socket socket{socket_fd};

            disable_sigpipe(
                socket.get()
            );

            sockaddr_in backend_address{};

            backend_address.sin_family =
                AF_INET;

            backend_address.sin_port =
                htons(backend.port);

            const int address_result =
                ::inet_pton(
                    AF_INET,
                    backend.address.c_str(),
                    &backend_address.sin_addr
                );

            if(address_result == 0){
                throw std::runtime_error(
                    "Invalid backend IP address: " +
                    backend.address
                );
            }

            if(address_result == -1){
                throw_system_error(
                    "inet_pton",
                    errno
                );
            }

            const int original_flags =
                ::fcntl(
                    socket.get(),
                    F_GETFL,
                    0
                );

            if(original_flags == -1){
                throw_system_error(
                    "fcntl(F_GETFL)",
                    errno
                );
            }

            if(::fcntl(
                socket.get(),
                F_SETFL,
                original_flags | O_NONBLOCK
            ) == -1){
                throw_system_error(
                    "fcntl(F_SETFL)",
                    errno
                );
            }

            const int connect_result =
                ::connect(
                    socket.get(),
                    reinterpret_cast<sockaddr*>(
                        &backend_address
                    ),
                    sizeof(backend_address)
                );

            if(connect_result == -1){
                const int errorcode =
                    errno;

                if(
                    errorcode != EINPROGRESS &&
                    errorcode != EINTR
                ){
                    return std::nullopt;
                }

                pollfd poll_fd{};

                poll_fd.fd =
                    socket.get();

                poll_fd.events =
                    POLLOUT;

                const int poll_result =
                    ::poll(
                        &poll_fd,
                        1,
                        static_cast<int>(
                            timeout.count()
                        )
                    );

                if(poll_result == 0){
                    return std::nullopt;
                }

                if(poll_result == -1){
                    if(errno == EINTR){
                        return std::nullopt;
                    }

                    throw_system_error(
                        "poll",
                        errno
                    );
                }

                int socket_error = 0;

                socklen_t error_length =
                    sizeof(socket_error);

                if(::getsockopt(
                    socket.get(),
                    SOL_SOCKET,
                    SO_ERROR,
                    &socket_error,
                    &error_length
                ) == -1){
                    throw_system_error(
                        "getsockopt(SO_ERROR)",
                        errno
                    );
                }

                if(socket_error != 0){
                    return std::nullopt;
                }
            }

            // restore blocking mode.
            if(::fcntl(
                socket.get(),
                F_SETFL,
                original_flags
            ) == -1){
                throw_system_error(
                    "fcntl(F_SETFL)",
                    errno
                );
            }

            return socket;
        }


        HealthCheckResult
        check_backend_health(
            const Backend& backend
        ){
            try{
                const std::optional<Socket> socket =
                    try_connect_to_backend(
                        backend,
                        health_check_timeout
                    );

                if(!socket.has_value()){
                    return
                        HealthCheckResult::unhealthy;
                }

                return
                    HealthCheckResult::healthy;
            }catch(const std::exception& error){
                std::cerr
                    << "Unable to perform health check for: "
                    << backend.address
                    << ':'
                    << backend.port
                    << ": "
                    << error.what()
                    << '\n';

                return
                    HealthCheckResult::check_failed;
            }
        }


        Backend& choose_backend(){
            const std::size_t start_index =
                next_backend_index.fetch_add(
                    1,
                    std::memory_order_relaxed
                );

            for(std::size_t offset = 0; offset < backends.size(); offset++){
                Backend& backend =
                    backends[
                        (start_index + offset)
                        % backends.size()
                    ];

                if(backend.healthy.load(
                    std::memory_order_relaxed
                )){
                    return backend;
                }
            }

            throw std::runtime_error(
                "No healthy backends available"
            );
        }

    } // anonymous namespace


    Backend::Backend(
        std::string address,
        std::uint16_t port
    )
        : address{std::move(address)},
        port{port},
        healthy{true}
    {}


    void set_backend_health(
        Backend& backend,
        bool is_healthy
    ){
        const bool previous_health =
            backend.healthy.exchange(
                is_healthy,
                std::memory_order_relaxed
            );

        if(previous_health != is_healthy){
            std::cout
                << "Backend "
                << backend.address
                << ':'
                << backend.port
                << " health changed. New status: "
                << (is_healthy? "healthy" : "unhealthy")
                << '\n';
        }
    }


    BackendConnection
    connect_to_healthy_backend(){
        for(std::size_t attempts = 0; attempts < backends.size(); attempts++){
            Backend& backend =
                choose_backend();

            std::optional<Socket> socket =
                try_connect_to_backend(
                    backend,
                    backend_connect_timeout
                );

            if(socket.has_value()){
                return BackendConnection{
                    &backend,
                    std::move(*socket)
                };
            }

            set_backend_health(
                backend,
                false
            );
        }

        throw std::runtime_error(
            "Unable to connect to any healthy backend"
        );
    }


    void health_check_loop(){
        while(!stop_requested.load(
            std::memory_order_relaxed
        )){
            for(Backend& backend : backends){
                const HealthCheckResult result =
                    check_backend_health(
                        backend
                    );

                if(result == HealthCheckResult::check_failed){
                    continue;
                }

                const bool is_healthy =
                    result ==
                    HealthCheckResult::healthy;

                set_backend_health(
                    backend,
                    is_healthy
                );
            }

            std::this_thread::sleep_for(
                std::chrono::seconds{2}
            );
        }
    }

}