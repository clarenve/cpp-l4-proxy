#include "server.hpp"

#include "backend.hpp"
#include "errors.hpp"
#include "proxy.hpp"
#include "shutdown.hpp"
#include "socket.hpp"

#include <cerrno>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

namespace l4{

    void run_server(
        std::uint16_t port
    ){
        Socket listening_socket =
            create_listening_socket(
                port
            );

        std::jthread health_check_thread{
            health_check_loop
        };

        std::vector<std::jthread>
            connection_threads;

        std::vector<std::weak_ptr<Socket>>
            active_client_sockets;


        const auto shutdown_server =
            [&active_client_sockets](){

            stop_requested.store(
                true,
                std::memory_order_relaxed
            );

            for(auto& weak_socket : active_client_sockets){
                if(auto socket = weak_socket.lock()){
                    if(socket->valid()){
                        ::shutdown(
                            socket->get(),
                            SHUT_RDWR
                        );
                    }
                }
            }
        };


        std::cout
            << "Server listening on port "
            << port
            << '\n';


        try {
            while(!shutdown_signal_received){
                sockaddr_in client_address{};

                socklen_t client_address_length =
                    sizeof(client_address);

                const int client_fd =
                    ::accept(
                        listening_socket.get(),
                        reinterpret_cast<sockaddr*>(
                            &client_address
                        ),
                        &client_address_length
                    );

                if(client_fd == -1){
                    if(errno == EINTR){
                        if(shutdown_signal_received){
                            break;
                        }

                        continue;
                    }

                    throw_system_error(
                        "accept",
                        errno
                    );
                }

                auto client_socket =
                    std::make_shared<Socket>(
                        client_fd
                    );

                disable_sigpipe(
                    client_socket->get()
                );

                print_client_address(
                    client_address
                );

                active_client_sockets
                    .emplace_back(
                        client_socket
                    );

                connection_threads.emplace_back(
                    [client_socket](){
                        try{
                            proxy_connection(
                                *client_socket
                            );
                        }catch(
                            const std::exception&
                                error
                        ){
                            std::cerr
                                << "Client connection error: "
                                << error.what()
                                << '\n';
                        }
                    }
                );
            }
        }
        catch(...){
            shutdown_server();

            throw;
        }


        shutdown_server();

        std::cout
            << "Server shutting down\n";
    }

}