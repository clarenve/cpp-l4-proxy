#include "kqueue_server.hpp"
#include "backend.hpp"

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
#include <arpa/inet.h>
#include <optional>
#include <stdexcept>
#include <cstring>

namespace l4{

    namespace{

        constexpr std::size_t buffer_size = 4096;
        constexpr int max_events = 64;

        struct DirectionalBuffer{
            std::array<char, buffer_size> data{};
            std::size_t size = 0;
            std::size_t offset = 0;
            bool read_eof = false;
        };

        struct Connection{
            Socket client_socket;
            Socket backend_socket;

            Backend* backend = nullptr;

            bool backend_connecting = false;

            DirectionalBuffer client_to_backend{};
            DirectionalBuffer backend_to_client{};
        };

        struct BackendConnectResult{
            Socket socket;
            bool connected_immediately;
        };

        std::optional<BackendConnectResult> start_backend_connect(Backend& backend){
            const int backend_fd = ::socket(AF_INET, SOCK_STREAM, 0);

            if(backend_fd == -1){
                throw_system_error("socket", errno);
            }

            Socket backend_socket{backend_fd};

            set_nonblocking(backend_socket.get());
            disable_sigpipe(backend_socket.get());

            sockaddr_in backend_address{};

            backend_address.sin_family = AF_INET;
            backend_address.sin_port = htons(backend.port);

            const int address_result = ::inet_pton(
                AF_INET,
                backend.address.c_str(),
                &backend_address.sin_addr
            );

            if(address_result == 0){
                throw std::runtime_error(
                    "Invalid backend IP address: " + backend.address
                );
            }

            if(address_result == -1){
                throw_system_error("inet_pton", errno);
            }

            const int connect_result = ::connect(
                backend_socket.get(),
                reinterpret_cast<sockaddr*>(&backend_address),
                sizeof(backend_address)
            );

            if(connect_result == 0){
                return BackendConnectResult{
                    std::move(backend_socket),
                    true
                };
            }

            const int errorcode = errno;

            if(errorcode == EINPROGRESS){
                return BackendConnectResult{
                    std::move(backend_socket),
                    false
                };
            }

            return std::nullopt;
        }

        int get_socket_error(int socket_fd){
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);

            if(::getsockopt(
                socket_fd,
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &error_length
            ) == -1){
                throw_system_error("getsockopt(SO_ERROR)", errno);
            }

            return socket_error;
        }

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

        void remove_read_event(int kqueue_fd, int socket_fd){
            struct kevent change{};

            EV_SET(
                &change,
                static_cast<uintptr_t>(socket_fd),
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

        void register_write_event(int kqueue_fd, int socket_fd){
            struct kevent change{};

            EV_SET(
                &change,
                static_cast<uintptr_t>(socket_fd),
                EVFILT_WRITE,
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
                throw_system_error("kevent(EV_ADD, EVFILT_WRITE)", errno);
            }
        }

        void remove_write_event(int kqueue_fd, int socket_fd){
            struct kevent change{};

            EV_SET(
                &change,
                static_cast<uintptr_t>(socket_fd),
                EVFILT_WRITE,
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

                throw_system_error("kevent(EV_DELETE, EVFILT_WRITE)", errno);
            }
        }

        bool read_direction(
            int kqueue_fd,
            int source_fd,
            int destination_fd,
            DirectionalBuffer& buffer
        ){
            if(buffer.read_eof || buffer.size != 0){
                return true;
            }

            while(true){
                const ssize_t received = ::recv(
                    source_fd,
                    buffer.data.data(),
                    buffer.data.size(),
                    0
                );

                if(received > 0){
                    buffer.size = static_cast<std::size_t>(received);
                    buffer.offset = 0;

                    remove_read_event(kqueue_fd, source_fd);
                    register_write_event(kqueue_fd, destination_fd);
                    return true;
                }

                if(received == 0){
                    buffer.read_eof = true;
                    remove_read_event(kqueue_fd, source_fd);

                    // Reads resume only after pending bytes have been sent.
                    if(::shutdown(destination_fd, SHUT_WR) == -1){
                        std::cerr
                            << "shutdown failed for fd "
                            << destination_fd
                            << ": "
                            << std::strerror(errno)
                            << '\n';

                        return false;
                    }

                    return true;
                }

                if(errno == EINTR){
                    continue;
                }

                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    return true;
                }

                std::cerr
                    << "recv failed for fd "
                    << source_fd
                    << ": "
                    << std::strerror(errno)
                    << '\n';

                return false;
            }
        }

        bool write_direction(
            int kqueue_fd,
            int source_fd,
            int destination_fd,
            DirectionalBuffer& buffer
        ){
            while(buffer.offset < buffer.size){
                const ssize_t sent = ::send(
                    destination_fd,
                    buffer.data.data() + buffer.offset,
                    buffer.size - buffer.offset,
                    0
                );

                if(sent > 0){
                    buffer.offset += static_cast<std::size_t>(sent);
                    continue;
                }

                if(sent == -1){
                    if(errno == EINTR){
                        continue;
                    }

                    if(errno == EAGAIN || errno == EWOULDBLOCK){
                        // Keep the offset and write registration for the next event.
                        return true;
                    }

                    std::cerr
                        << "send failed for fd "
                        << destination_fd
                        << ": "
                        << std::strerror(errno)
                        << '\n';
                }else{
                    std::cerr
                        << "send made no progress for fd "
                        << destination_fd
                        << '\n';
                }

                return false;
            }

            buffer.size = 0;
            buffer.offset = 0;

            remove_write_event(kqueue_fd, destination_fd);

            if(!buffer.read_eof){
                register_read_event(kqueue_fd, source_fd);
            }

            return true;
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

        std::unordered_map<int, std::unique_ptr<Connection>> connections;
        std::unordered_map<int, Connection*> fd_to_connection;

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

                            Backend& backend = choose_backend();

                            std::optional<BackendConnectResult> backend_result =
                                start_backend_connect(backend);

                            if(!backend_result.has_value()){
                                set_backend_health(backend, false);

                                std::cerr
                                    << "Unable to start backend connection to "
                                    << backend.address
                                    << ':'
                                    << backend.port
                                    << '\n';

                                continue;
                            }

                            const int backend_fd = backend_result->socket.get();

                            auto connection = std::make_unique<Connection>(
                                Connection{
                                    std::move(client_socket),
                                    std::move(backend_result->socket),
                                    &backend,
                                    !backend_result->connected_immediately
                                }
                            );

                            Connection* connection_ptr = connection.get();

                            connections.emplace(
                                client_fd,
                                std::move(connection)
                            );

                            fd_to_connection.emplace(
                                client_fd,
                                connection_ptr
                            );

                            fd_to_connection.emplace(
                                backend_fd,
                                connection_ptr
                            );

                            if(connection_ptr->backend_connecting){
                                register_write_event(
                                    kqueue_socket.get(),
                                    backend_fd
                                );

                                std::cout
                                    << "Backend connection in progress for client fd "
                                    << client_fd
                                    << '\n';
                            }else{
                                register_read_event(kqueue_socket.get(), client_fd);
                                register_read_event(kqueue_socket.get(), backend_fd);

                                std::cout
                                    << "Backend connected immediately for client fd "
                                    << client_fd
                                    << '\n';
                            }
                        }

                    }

                    // Client or backend socket event.

                    auto connection_it = fd_to_connection.find(event_fd);

                    if(connection_it == fd_to_connection.end()){
                        continue;
                    }

                    Connection& connection = *connection_it->second;

                    if(event_fd == connection.backend_socket.get() &&
                        event.filter == EVFILT_WRITE &&
                        connection.backend_connecting
                    ){
                        const int socket_error =
                            get_socket_error(connection.backend_socket.get());

                        remove_write_event(
                            kqueue_socket.get(),
                            connection.backend_socket.get()
                        );

                        if(socket_error != 0){
                            std::cerr
                                << "Backend connection failed for "
                                << connection.backend->address
                                << ':'
                                << connection.backend->port
                                << ": "
                                << std::strerror(socket_error)
                                << '\n';

                            set_backend_health(
                                *connection.backend,
                                false
                            );

                            const int client_fd =
                                connection.client_socket.get();

                            const int backend_fd =
                                connection.backend_socket.get();

                            fd_to_connection.erase(client_fd);
                            fd_to_connection.erase(backend_fd);

                            connections.erase(client_fd);

                            continue;
                        }

                        connection.backend_connecting = false;

                        register_read_event(kqueue_socket.get(), connection.client_socket.get());
                        register_read_event(kqueue_socket.get(), connection.backend_socket.get());

                        std::cout
                            << "Backend connected for client fd "
                            << connection.client_socket.get()
                            << '\n';

                        continue;
                    }

                    if(connection.backend_connecting){
                        continue;
                    }

                    const int client_fd = connection.client_socket.get();
                    const int backend_fd = connection.backend_socket.get();
                    const bool from_client = event_fd == client_fd;

                    bool success = true;

                    if(event.filter == EVFILT_READ){
                        const int destination_fd = from_client ? backend_fd : client_fd;

                        DirectionalBuffer& buffer = from_client
                            ? connection.client_to_backend
                            : connection.backend_to_client;

                        success = read_direction(
                            kqueue_socket.get(),
                            event_fd,
                            destination_fd,
                            buffer
                        );
                    }else if(event.filter == EVFILT_WRITE){
                        const int source_fd = from_client ? backend_fd : client_fd;

                        DirectionalBuffer& buffer = from_client
                            ? connection.backend_to_client
                            : connection.client_to_backend;

                        success = write_direction(
                            kqueue_socket.get(),
                            source_fd,
                            event_fd,
                            buffer
                        );
                    }else{
                        continue;
                    }

                    const bool finished =
                        connection.client_to_backend.read_eof &&
                        connection.backend_to_client.read_eof;

                    if(!success || finished){
                        fd_to_connection.erase(client_fd);
                        fd_to_connection.erase(backend_fd);

                        // Socket destruction closes both fds and removes their filters.
                        connections.erase(client_fd);
                    }

                }

        }

        std::cout << "kqueue server shutting down\n";

    }


}