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
#include <algorithm>
#include <chrono>

namespace l4{

    namespace{

        using Clock = std::chrono::steady_clock;

        constexpr auto connect_timeout = std::chrono::seconds{1};

        enum class ConnectionState{
            connecting,
            established
        };

        constexpr std::size_t buffer_size = 4096;
        constexpr std::size_t high_watermark = 64 * 1024;
        constexpr std::size_t low_watermark = 16 * 1024;
        static_assert(low_watermark < high_watermark);
        constexpr int max_events = 64;

        struct DirectionalBuffer{
            std::array<char, high_watermark> data{};
            std::size_t size = 0;
            std::size_t offset = 0;
            bool read_eof = false;
            bool read_paused = false;
            bool write_shutdown = false;
            int errorcode = 0;

            std::size_t pending() const noexcept{
                return size - offset;
            }

            bool finished() const noexcept{
                return errorcode == 0 &&
                    read_eof &&
                    pending() == 0 &&
                    write_shutdown;
            }
        };

        struct Connection{
            Socket client_socket;
            Socket backend_socket;

            Backend* backend = nullptr;

            ConnectionState state = ConnectionState::connecting;

            std::vector<Backend*> candidates;
            std::size_t next_candidate = 0;
            Clock::time_point connect_deadline{};

            DirectionalBuffer client_to_backend{};
            DirectionalBuffer backend_to_client{};
        };

        struct BackendConnectResult{
            Socket socket;
            bool connected_immediately = false;
            int errorcode = 0;
        };

        BackendConnectResult start_backend_connect(Backend& backend){
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

            if(errorcode == EINPROGRESS || errorcode == EINTR){
                return BackendConnectResult{
                    std::move(backend_socket),
                    false
                };
            }

            // Local resource or permission failures are not backend health failures.
            switch(errorcode){
                case EADDRNOTAVAIL:
                case ENOBUFS:
                case ENOMEM:
                case EACCES:
                case EPERM:
                    throw_system_error("connect", errorcode);
                default:
                    break;
            }

            return BackendConnectResult{Socket{}, false, errorcode};
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

        bool fail_direction(
            DirectionalBuffer& buffer,
            int socket_fd,
            const char* operation,
            int errorcode
        ){
            buffer.errorcode = errorcode;

            std::cerr
                << operation
                << " failed for fd "
                << socket_fd
                << ": "
                << std::strerror(errorcode)
                << '\n';

            return false;
        }

        bool finish_direction(int destination_fd, DirectionalBuffer& buffer){
            if(buffer.errorcode != 0){
                return false;
            }

            if(!buffer.read_eof || buffer.pending() != 0 || buffer.write_shutdown){
                return true;
            }

            while(::shutdown(destination_fd, SHUT_WR) == -1){
                if(errno == EINTR){
                    continue;
                }

                return fail_direction(buffer, destination_fd, "shutdown", errno);
            }

            buffer.write_shutdown = true;
            return true;
        }

        int event_error(const struct kevent& event){
            if((event.flags & EV_ERROR) && event.data != 0){
                return static_cast<int>(event.data);
            }

            if((event.flags & EV_EOF) && event.fflags != 0){
                return static_cast<int>(event.fflags);
            }

            return 0;
        }

        bool read_direction(
            int kqueue_fd,
            int source_fd,
            int destination_fd,
            DirectionalBuffer& buffer
        ){
            if(buffer.errorcode != 0){
                return false;
            }

            if(buffer.read_eof || buffer.read_paused){
                return true;
            }

            if(buffer.offset != 0){
                const std::size_t pending = buffer.pending();

                std::memmove(
                    buffer.data.data(),
                    buffer.data.data() + buffer.offset,
                    pending
                );

                buffer.size = pending;
                buffer.offset = 0;
            }

            while(true){
                if(buffer.pending() >= high_watermark){
                    remove_read_event(kqueue_fd, source_fd);
                    buffer.read_paused = true;
                    return true;
                }

                const std::size_t available = std::min(
                    buffer_size,
                    high_watermark - buffer.size
                );

                const ssize_t received = ::recv(
                    source_fd,
                    buffer.data.data() + buffer.size,
                    available,
                    0
                );

                if(received > 0){
                    const bool was_empty = buffer.pending() == 0;

                    buffer.size += static_cast<std::size_t>(received);

                    if(was_empty){
                        register_write_event(kqueue_fd, destination_fd);
                    }

                    continue;
                }

                if(received == 0){
                    buffer.read_eof = true;
                    remove_read_event(kqueue_fd, source_fd);
                    return finish_direction(destination_fd, buffer);
                }

                if(errno == EINTR){
                    continue;
                }

                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    return true;
                }

                return fail_direction(buffer, source_fd, "recv", errno);
            }
        }

        bool write_direction(
            int kqueue_fd,
            int source_fd,
            int destination_fd,
            DirectionalBuffer& buffer
        ){
            if(buffer.errorcode != 0){
                return false;
            }

            // A notification may remain in the current batch after its filter is removed.
            if(buffer.pending() == 0){
                return finish_direction(destination_fd, buffer);
            }

            while(buffer.offset < buffer.size){
                const ssize_t sent = ::send(
                    destination_fd,
                    buffer.data.data() + buffer.offset,
                    buffer.size - buffer.offset,
                    0
                );

                if(sent > 0){
                    buffer.offset += static_cast<std::size_t>(sent);

                    if(buffer.read_paused &&
                        !buffer.read_eof &&
                        buffer.pending() <= low_watermark
                    ){
                        register_read_event(kqueue_fd, source_fd);
                        buffer.read_paused = false;
                    }

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

                    return fail_direction(buffer, destination_fd, "send", errno);
                }

                return fail_direction(buffer, destination_fd, "send (no progress)", EIO);
            }

            buffer.size = 0;
            buffer.offset = 0;

            remove_write_event(kqueue_fd, destination_fd);

            return finish_direction(destination_fd, buffer);
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

        std::vector<std::unique_ptr<Connection>> retired_connections;
        retired_connections.reserve(max_events);

        std::vector<Socket> retired_backend_sockets;

        const auto retire_connection = [&](Connection& connection){
            const int client_fd = connection.client_socket.get();
            const int backend_fd = connection.backend_socket.get();

            fd_to_connection.erase(client_fd);
            fd_to_connection.erase(backend_fd);

            auto connection_it = connections.find(client_fd);

            if(connection_it != connections.end()){
                retired_connections.push_back(std::move(connection_it->second));
                connections.erase(connection_it);
            }
        };

        const auto establish_connection = [&](Connection& connection){
            register_read_event(kqueue_socket.get(), connection.client_socket.get());
            register_read_event(kqueue_socket.get(), connection.backend_socket.get());

            connection.state = ConnectionState::established;
            set_backend_health(*connection.backend, true);

            std::cout
                << "Backend connected to "
                << connection.backend->address
                << ':'
                << connection.backend->port
                << " for client fd "
                << connection.client_socket.get()
                << '\n';
        };

        const auto report_connect_failure = [&](Connection& connection, int errorcode){
            std::cerr
                << "Backend connection failed for "
                << connection.backend->address
                << ':'
                << connection.backend->port
                << " (client fd "
                << connection.client_socket.get()
                << "): "
                << std::strerror(errorcode)
                << '\n';

            set_backend_health(*connection.backend, false);
        };

        const auto report_setup_error = [&](Connection& connection, const std::exception& error){
            std::cerr
                << "Backend setup error for client fd "
                << connection.client_socket.get()
                << ": "
                << error.what()
                << '\n';
        };

        const auto try_next_backend = [&](Connection& connection){
            if(connection.state != ConnectionState::connecting){
                return false;
            }

            try{
                while(connection.next_candidate < connection.candidates.size()){
                    connection.backend = connection.candidates[connection.next_candidate++];
                    connection.connect_deadline = Clock::now() + connect_timeout;

                    BackendConnectResult result = start_backend_connect(*connection.backend);

                    if(result.errorcode != 0){
                        report_connect_failure(connection, result.errorcode);
                        continue;
                    }

                    connection.backend_socket = std::move(result.socket);
                    const int backend_fd = connection.backend_socket.get();
                    fd_to_connection.emplace(backend_fd, &connection);

                    if(result.connected_immediately){
                        establish_connection(connection);
                    }else{
                        register_write_event(kqueue_socket.get(), backend_fd);

                        std::cout
                            << "Backend connection in progress to "
                            << connection.backend->address
                            << ':'
                            << connection.backend->port
                            << " for client fd "
                            << connection.client_socket.get()
                            << '\n';
                    }

                    return true;
                }

                std::cerr
                    << "All backend candidates exhausted for client fd "
                    << connection.client_socket.get()
                    << '\n';
            }catch(const std::exception& error){
                report_setup_error(connection, error);
            }

            return false;
        };

        const auto retry_backend = [&](Connection& connection, int errorcode){
            if(connection.state != ConnectionState::connecting){
                return false;
            }

            try{
                const int backend_fd = connection.backend_socket.get();

                remove_write_event(kqueue_socket.get(), backend_fd);
                fd_to_connection.erase(backend_fd);
                retired_backend_sockets.push_back(std::move(connection.backend_socket));

                report_connect_failure(connection, errorcode);
            }catch(const std::exception& error){
                report_setup_error(connection, error);
                return false;
            }

            return try_next_backend(connection);
        };

        std::vector<struct kevent> events(max_events);

        std::cout
            << "kqueue server listening on port: "
            << port
            << '\n';

        while(!shutdown_signal_received){
            std::optional<Clock::time_point> earliest_deadline;

            for(const auto& entry : connections){
                const Connection& connection = *entry.second;

                if(connection.state == ConnectionState::connecting &&
                    (!earliest_deadline || connection.connect_deadline < *earliest_deadline)
                ){
                    earliest_deadline = connection.connect_deadline;
                }
            }

            timespec timeout{};
            const timespec* timeout_ptr = nullptr;

            if(earliest_deadline){
                const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::max(*earliest_deadline - Clock::now(), Clock::duration::zero())
                );
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);

                timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(seconds.count());
                timeout.tv_nsec = static_cast<decltype(timeout.tv_nsec)>((remaining - seconds).count());
                timeout_ptr = &timeout;
            }

            const int event_count = 
                ::kevent(
                    kqueue_socket.get(),
                    nullptr,
                    0,
                    events.data(),
                    static_cast<int>(events.size()),
                    timeout_ptr
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
                        const int errorcode = event_error(event);

                        if(errorcode != 0){
                            throw_system_error("kevent(listener)", errorcode);
                        }

                        if(event.flags & EV_ERROR || event.filter != EVFILT_READ){
                            continue;
                        }

                        for(int accepted = 0; accepted < max_events; accepted++){
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
                            std::unique_ptr<Connection> connection;

                            try{
                                set_nonblocking(client_socket.get());
                                disable_sigpipe(client_socket.get());
                                print_client_address(client_address);

                                connection = std::make_unique<Connection>();
                                connection->client_socket = std::move(client_socket);
                                connection->candidates = backend_candidates();
                            }catch(const std::exception& error){
                                std::cerr
                                    << "Client setup error: "
                                    << error.what()
                                    << '\n';
                                continue;
                            }

                            Connection* connection_ptr = connection.get();
                            connections.emplace(client_fd, std::move(connection));
                            fd_to_connection.emplace(client_fd, connection_ptr);

                            if(!try_next_backend(*connection_ptr)){
                                retire_connection(*connection_ptr);
                            }
                        }

                    }

                    // Client or backend socket event.

                    auto connection_it = fd_to_connection.find(event_fd);

                    if(connection_it == fd_to_connection.end()){
                        continue;
                    }

                    Connection& connection = *connection_it->second;
                    const int errorcode = event_error(event);

                    if(event.flags & EV_ERROR){
                        if(errorcode != 0){
                            std::cerr
                                << "kevent failed for fd "
                                << event_fd
                                << ": "
                                << std::strerror(errorcode)
                                << '\n';

                            retire_connection(connection);
                        }

                        continue;
                    }

                    if(connection.state == ConnectionState::connecting){
                        if(event_fd != connection.backend_socket.get() || event.filter != EVFILT_WRITE){
                            continue;
                        }

                        try{
                            if(Clock::now() >= connection.connect_deadline){
                                if(!retry_backend(connection, ETIMEDOUT)){
                                    retire_connection(connection);
                                }

                                continue;
                            }

                            const int connect_error = get_socket_error(connection.backend_socket.get());
                            const int effective_error = connect_error != 0 ? connect_error : errorcode;

                            if(effective_error != 0){
                                if(!retry_backend(connection, effective_error)){
                                    retire_connection(connection);
                                }
                            }else{
                                remove_write_event(kqueue_socket.get(), connection.backend_socket.get());
                                establish_connection(connection);
                            }
                        }catch(const std::exception& error){
                            report_setup_error(connection, error);
                            retire_connection(connection);
                        }

                        continue;
                    }

                    const int client_fd = connection.client_socket.get();
                    const int backend_fd = connection.backend_socket.get();
                    const bool from_client = event_fd == client_fd;

                    if(errorcode != 0){
                        DirectionalBuffer& buffer = event.filter == EVFILT_WRITE
                            ? (from_client ? connection.backend_to_client : connection.client_to_backend)
                            : (from_client ? connection.client_to_backend : connection.backend_to_client);

                        fail_direction(buffer, event_fd, "kevent(socket)", errorcode);
                        retire_connection(connection);
                        continue;
                    }

                    bool success = true;

                    if(event.filter == EVFILT_READ){
                        const int destination_fd = from_client ? backend_fd : client_fd;

                        DirectionalBuffer& buffer = from_client
                            ? connection.client_to_backend
                            : connection.backend_to_client;

                        // EV_EOF may accompany unread data. Only recv() == 0 completes the source.
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

                        if((event.flags & EV_EOF) && buffer.pending() != 0){
                            success = fail_direction(buffer, event_fd, "kevent(write EOF)", EPIPE);
                        }else{
                            success = write_direction(
                                kqueue_socket.get(),
                                source_fd,
                                event_fd,
                                buffer
                            );
                        }
                    }else{
                        continue;
                    }

                    const bool finished =
                        connection.client_to_backend.finished() &&
                        connection.backend_to_client.finished();

                    if(!success || finished){
                        retire_connection(connection);
                        continue;
                    }

                }

            const auto now = Clock::now();
            std::vector<Connection*> expired_connections;

            for(const auto& entry : connections){
                Connection& connection = *entry.second;

                if(connection.state == ConnectionState::connecting && connection.connect_deadline <= now){
                    expired_connections.push_back(&connection);
                }
            }

            for(Connection* connection : expired_connections){
                if(!retry_backend(*connection, ETIMEDOUT)){
                    retire_connection(*connection);
                }
            }

            // Keep retired fds open until no stale events from this batch remain.
            // Closing the sockets also removes their remaining kqueue filters.
            retired_connections.clear();
            retired_backend_sockets.clear();
        }

        std::cout << "kqueue server shutting down\n";

    }


}
