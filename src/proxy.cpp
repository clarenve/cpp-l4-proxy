#include "proxy.hpp"

#include "backend.hpp"
#include "errors.hpp"
#include "shutdown.hpp"

#include <array>
#include <cerrno>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>

namespace l4{

    namespace{

        constexpr std::size_t buffer_size = 4096;


        enum class ForwardDirection{
            client_to_backend,
            backend_to_client
        };


        struct SendResult{
            bool success;
            int errorcode;
        };


        SendResult send_all(int socket_fd, std::span<const char> data){
            std::size_t total_sent = 0;

            while(total_sent < data.size()){
                const ssize_t bytes_sent =
                    ::send(
                        socket_fd,
                        data.data() + total_sent,
                        data.size() - total_sent,
                        0
                    );

                if(bytes_sent == -1){
                    if(errno == EINTR){
                        continue;
                    }

                    return SendResult{
                        false,
                        errno
                    };
                }

                if(bytes_sent == 0){
                    return SendResult{
                        false,
                        0
                    };
                }

                total_sent +=
                    static_cast<std::size_t>(
                        bytes_sent
                    );
            }

            return SendResult{
                true,
                0
            };
        }


        void forward_data(
            int source_fd,
            int destination_fd,
            Backend& backend,
            ForwardDirection direction
        ){
            try{
                std::array<char, buffer_size>
                    buffer{};

                while(true){
                    const ssize_t bytes_received =
                        ::recv(
                            source_fd,
                            buffer.data(),
                            buffer.size(),
                            0
                        );

                    if(bytes_received == -1){
                        if(errno == EINTR){
                            continue;
                        }

                        const int errorcode =
                            errno;

                        if(direction == ForwardDirection::backend_to_client){
                            switch(errorcode){
                                case ECONNRESET:
                                case ETIMEDOUT:
                                case ENETRESET:
                                    set_backend_health(
                                        backend,
                                        false
                                    );
                                    break;

                                default:
                                    break;
                            }
                        }

                        throw_system_error(
                            "recv",
                            errorcode
                        );
                    }

                    if(bytes_received == 0){
                        if(stop_requested.load(std::memory_order_relaxed)){
                            if(::shutdown(
                                destination_fd,
                                SHUT_RDWR
                            ) == -1){
                                throw_system_error(
                                    "shutdown",
                                    errno
                                );
                            }

                            return;
                        }

                        if(::shutdown(
                            destination_fd,
                            SHUT_WR
                        ) == -1){
                            throw_system_error(
                                "shutdown",
                                errno
                            );
                        }

                        return;
                    }

                    const SendResult send_result =
                        send_all(
                            destination_fd,
                            std::span<const char>{
                                buffer.data(),
                                static_cast<std::size_t>(
                                    bytes_received
                                )
                            }
                        );

                    if(!send_result.success){
                        if(direction == ForwardDirection::client_to_backend){
                            switch(
                                send_result.errorcode
                            ){
                                case EPIPE:
                                case ECONNRESET:
                                case ETIMEDOUT:
                                case ENETRESET:
                                    set_backend_health(
                                        backend,
                                        false
                                    );
                                    break;

                                default:
                                    break;
                            }
                        }

                        if(send_result.errorcode != 0){
                            throw_system_error(
                                "send",
                                send_result.errorcode
                            );
                        }

                        throw std::runtime_error(
                            "send made no progress"
                        );
                    }
                }
            }catch(const std::exception& error){
                std::cerr
                    << "Forwarding error: "
                    << error.what()
                    << '\n';

                ::shutdown(
                    source_fd,
                    SHUT_RDWR
                );

                ::shutdown(
                    destination_fd,
                    SHUT_RDWR
                );
            }
        }

    } // anonymous namespace


    void proxy_connection(Socket& client_socket){
        BackendConnection connection =
            connect_to_healthy_backend();

        Backend& backend =
            *connection.backend;

        std::cout
            << "Selected backend: "
            << backend.address
            << ':'
            << backend.port
            << '\n';

        std::thread client_to_backend{
            forward_data,
            client_socket.get(),
            connection.socket.get(),
            std::ref(backend),
            ForwardDirection::
                client_to_backend
        };

        std::thread backend_to_client{
            forward_data,
            connection.socket.get(),
            client_socket.get(),
            std::ref(backend),
            ForwardDirection::
                backend_to_client
        };

        client_to_backend.join();
        backend_to_client.join();
    }

}