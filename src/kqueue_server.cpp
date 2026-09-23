#include "kqueue_server.hpp"

#include "errors.hpp"
#include "shutdown.hpp"
#include "socket.hpp"

#include <cerrno>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <unordered_map>
#include <array>
#include <cstdint>

namespace l4{

    namespace{

        constexpr std::size_t buffer_size = 4096;
        constexpr int max_events = 64;

        struct ClientConnection{
            Socket socket;
        };

        void register_read_event(int kqueue_fd, int socket_fd){
            struct kevent change{};

            EV_SET(
                &change,
                static_cast<uintptr_t>(socket_fd),
                EVFILT_READ,
                EV_ADD,
                0,
                0,
                nullptr
            );

            if(::kevent(
                kqueue_fd,
                &change,
                1,
                nullptr,
                0,
                nullptr
            ) == -1){
                throw_system_error("kevent(EV_ADD, EVFILT_READ)", errno);
            }
        }

        void remove_read_event(int kqueue_fd, int client_fd){
            struct kevent change{};

            EV_SET(
                &change,
                static_cast<uintptr_t>(client_fd),
                EVFILT_READ,
                EV_DELETE,
                0,
                0,
                nullptr
            );

            if(::kevent(
                kqueue_fd,
                &change,
                1,
                nullptr,
                0,
                nullptr
            ) == -1){
                if(errno == ENOENT){
                    return;
                }

                throw_system_error("kevent(EV_DELETE, EVFILT_READ)", errno);
            }
        }
    }

    void run_kqueue_server(std::uint16_t port){
        Socket listening_socket = create_listening_socket(port);

        set_nonblocking(listening_socket.get());

        const int kqueue_fd = ::kqueue();

        if(kqueue_fd == -1){
            throw_system_error(
                "kqueue",
                errno
            );
        }

        Socket kqueue_socket{kqueue_fd};

        register_read_event(kqueue_socket.get(), listening_socket.get());

        std::unordered_map<int, std::unique_ptr<ClientConnection>> clients;

        std::vector<struct kevent> events(max_events);

        std::cout
            << "kqueue server listening on port: "
            << port
            << '\n';

        while(!shutdown_signal_received){
            const int event_count = 
                ::kevent(
                    kqueue_socket.get(),
                    nullptr,
                    0,
                    events.data(),
                    static_cast<int>(events.size()),
                    nullptr
                );
            
                if(event_count == -1){
                    if(errno == EINTR){
                        if(shutdown_signal_received){
                            break;
                        }

                        continue;
                    }

                    throw_system_error("kevent(wait)", errno);
                }

                for(int i = 0; i < event_count; i++){
                    const struct kevent& event = events[i];

                    const int event_fd = static_cast<int>(event.ident);

                    if(event_fd == listening_socket.get()){ //accept new incoming clients
                        while(1){
                            sockaddr_in client_address{};

                            socklen_t client_address_length = sizeof(client_address);

                            const int client_fd = 
                                ::accept(
                                    listening_socket.get(),
                                    reinterpret_cast<sockaddr*>(&client_address),
                                    &client_address_length
                                );
                            
                            if(client_fd == -1){
                                if(errno == EAGAIN || errno == EWOULDBLOCK){
                                    break;
                                }

                                if(errno == EINTR){
                                    continue;
                                }

                                throw_system_error("accept", errno);
                            }

                            Socket client_socket{client_fd};

                            set_nonblocking(client_socket.get());

                            disable_sigpipe(client_socket.get());

                            print_client_address(client_address);

                            register_read_event(kqueue_socket.get(), client_socket.get());

                            clients.emplace(
                                client_fd,
                                std::make_unique<ClientConnection>(
                                    ClientConnection{std::move(client_socket)}
                                )
                            );

                            std::cout
                                << "Registered client fd: "
                                << client_fd
                                << " with kqueue\n";
                        }

                        continue;
                    }

                    //client socket event

                    auto client_it = clients.find(event_fd);

                    if(client_it == clients.end()){
                        continue;
                    }

                    ClientConnection& client = *client_it->second;

                    std::array<char, buffer_size> buffer{};

                    while(1){
                        const ssize_t bytes_received = 
                            ::recv(
                                client.socket.get(),
                                buffer.data(),
                                buffer.size(),
                                0
                            );

                        if(bytes_received > 0){
                            std::cout
                                << "Received "
                                << bytes_received
                                << " bytes from client fd "
                                << client.socket.get()
                                << '\n';

                            continue;
                        }

                        if(bytes_received == 0){
                            std::cout
                                << "Client fd "
                                << client.socket.get()
                                << " closed connection\n";

                                
                            remove_read_event(
                                kqueue_socket.get(),
                                client.socket.get()
                            );

                            clients.erase(client_it);

                            break;
                        }

                        if(bytes_received == -1){
                            if(errno == EAGAIN || errno == EWOULDBLOCK){
                                break;
                            }else if(errno == EINTR){
                                continue;
                            }

                            std::cerr
                                << "recv failed for client fd "         
                                << client.socket.get()
                                << '\n';
                                
                            remove_read_event(kqueue_socket.get(), client.socket.get());
                            clients.erase(client_it);

                            break;
                        }
                    
                    }

                }

        }

        std::cout << "kqueue server shutting down\n";

    }


}