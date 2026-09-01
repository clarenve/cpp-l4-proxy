#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <array>
#include <atomic>
#include <utility>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <optional>
#include <functional>
#include <vector>
#include <system_error>
#include <csignal>
#include <signal.h>
#include <memory>

namespace{

    constexpr std::uint16_t server_port = 8080;
    //constexpr std::uint16_t backend_port = 9000;
    constexpr int listen_backlog = 128; //how many pending connections may queue, waiting for accept()
    constexpr std::size_t buffer_size = 4096;

    constexpr auto backend_connect_timeout = std::chrono::milliseconds{1000};
    constexpr auto health_check_timeout = std::chrono::milliseconds{500};

    volatile std::sig_atomic_t shutdown_signal_received = 0;

    std::atomic<bool> stop_requested{false};

    enum class ForwardDirection{
        client_to_backend,
        backend_to_client
    };

    [[noreturn]] void throw_system_error(const std::string& operation, int errorcode){
        throw std::system_error(
            errorcode,
            std::generic_category(),
            operation
        );
        
        // throw std::runtime_error(
        //     operation + " failed " + std::strerror(errorcode)
        // );
    }

    void handle_shutdown_signal(int){
        shutdown_signal_received = 1;
    }

    class Socket{
    private:
        static constexpr int invalid_fd = -1;
        
        int fd_ = invalid_fd;

    public:
        Socket() noexcept = default;

        explicit Socket(int fd) noexcept
            : fd_{fd}
        {}

        ~Socket(){
            reset();
        }
        
        //copy constructor
        Socket(const Socket&) = delete;

        //copy assignment
        Socket& operator=(const Socket&) = delete;

        //move constructor
        Socket(Socket&& other) noexcept
            : fd_{other.fd_}
        {
            other.fd_ = invalid_fd;
        }

        //move assignment
        Socket& operator=(Socket&& other) noexcept{
            if(this != &other){
                //protection against self move assignment
                reset();

                fd_ = other.fd_;
                other.fd_ = invalid_fd;
            }

            return *this;
        }

        int get() const noexcept{
            return fd_;
        }

        bool valid() const noexcept{
            return fd_ != invalid_fd;
        }

        int release() noexcept{
            const int fd = fd_;
            fd_ = invalid_fd;
            return fd;
        }

        void reset(int new_fd = invalid_fd) noexcept{
            //assign this socket's fd_ with new_fd. assign -1 by default when called without arguments
            if(fd_ != invalid_fd && fd_ != new_fd){
                ::close(fd_);
            }

            fd_ = new_fd;
        }
    };

    struct Backend{
        std::string address;
        std::uint16_t port;
        std::atomic<bool> healthy{true};

        Backend(std::string address, std::uint16_t port)
            : address{std::move(address)},
            port{port},
            healthy{true}
        {}
    };

    struct BackendConnection{
        Backend* backend;
        Socket socket;
    };

    struct SendResult{
        bool success;
        int errorcode;
    };

    std::array<Backend, 3> backends{
        Backend{"127.0.0.1", 9001},
        Backend{"127.0.0.1", 9002},
        Backend{"127.0.0.1", 9003}
    };

    std::atomic<std::size_t> next_backend_index{0};

    enum class HealthCheckResult{
        healthy,
        unhealthy,
        check_failed
    };

    void disable_sigpipe(int socket_fd){
        const int value = 1;

        if(::setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_NOSIGPIPE,
            &value,
            sizeof(value)) == -1){
                throw_system_error("setsockopt(SO_NOSIGPIPE)", errno);
            }
    }

    std::optional<Socket> try_connect_to_backend(const Backend& backend, std::chrono::milliseconds timeout){
        const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);

        if(socket_fd == -1){
            throw_system_error("socket", errno);
        }

        Socket socket{socket_fd};

        disable_sigpipe(socket.get());

        sockaddr_in backend_address{};
        backend_address.sin_family = AF_INET;
        backend_address.sin_port = htons(backend.port);

        const int address_result = inet_pton(
            AF_INET,
            backend.address.c_str(),
            &backend_address.sin_addr);

        if(address_result == 0){ //stored backend ip address is bad/invalid
            //no need for errno cause address_reuslt == 0 means invalid ip. errno used if inet_pton returns -1

            throw std::runtime_error(
                "Invalid backend IP address: " + backend.address);
            
        }

        if(address_result == -1){
            throw_system_error("inet_pton", errno);
        }

        const int original_flags = ::fcntl(socket.get(), F_GETFL, 0); //get socket's current file status-flags

        if(original_flags == -1){
            throw_system_error("fcntl(F_GETFL)", errno);
        }

        if(::fcntl(
            socket.get(),
            F_SETFL,
            original_flags | O_NONBLOCK) == -1){
                throw_system_error("fcntl(F_SETFL)", errno);
            }
        

        const int connect_result = ::connect(
            socket.get(),
            reinterpret_cast<sockaddr*>(&backend_address),
            sizeof(backend_address)
        );
        
        if(connect_result == -1){
            const int errorcode = errno;

            if(errorcode != EINPROGRESS && errorcode != EINTR){
                return std::nullopt;
            }
            
            pollfd poll_fd{};
            poll_fd.fd = socket.get();
            poll_fd.events = POLLOUT;

            const int poll_result = ::poll(&poll_fd, 1, static_cast<int>(timeout.count()));

            if(poll_result == 0){
                //timeout
                
                return std::nullopt;
            }

            if(poll_result == -1){
                if(errno == EINTR){
                    return std::nullopt;
                }else{
                    throw_system_error("poll", errno);
                }
            }

            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);

            if(::getsockopt(
                socket.get(),
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &error_length) == -1){
                    throw_system_error("getsockopt(SO_ERROR)", errno);
                }
            
            if(socket_error != 0){
                return std::nullopt;
            }
        }

            //socket_error == 0 or connect_result == 0 means connection successful
            if(::fcntl(
                socket.get(),
                F_SETFL,
                original_flags) == -1){
                    throw_system_error("fcntl(F_SETFL)", errno);
                }
            
            return socket;
    }

    HealthCheckResult check_backend_health(const Backend& backend){
        try{
            const std::optional<Socket> socket = try_connect_to_backend(backend, health_check_timeout);

            if(!socket.has_value()){
                return HealthCheckResult::unhealthy;
            }
            
            return HealthCheckResult::healthy;

        }catch(const std::exception& error){
            std::cerr
                << "Unable to perform health check for: "
                << backend.address
                << ':'
                << backend.port
                << ": "
                << error.what()
                << '\n';

            return HealthCheckResult::check_failed;
        }

    }

    Backend& choose_backend(){
        const std::size_t start_index = next_backend_index.fetch_add(
            1,
            std::memory_order_relaxed);

        for(std::size_t offset = 0; offset < backends.size(); offset++){
            Backend& backend = backends[(start_index + offset) % backends.size()];

            if(backend.healthy.load(std::memory_order_relaxed)){
                return backend;
            }
        }

        throw std::runtime_error("No healthy backends available");
    }

    void set_backend_health(Backend& backend, bool is_healthy){
        const bool previous_health = backend.healthy.exchange(
            is_healthy,
            std::memory_order_relaxed);
        
        if(previous_health != is_healthy){
            std::cout
                << "Backend "
                << backend.address
                << ':'
                << backend.port
                << " health changed. New status: "
                << (is_healthy ? "healthy" : "unhealthy")
                << '\n';
        }
    }

    void health_check_loop(){
        while(!stop_requested.load(std::memory_order_relaxed)){
            for(Backend& backend : backends){
                HealthCheckResult result = check_backend_health(backend);

                if(result == HealthCheckResult::check_failed){
                    continue;
                }

                const bool is_healthy = result == HealthCheckResult::healthy;

                set_backend_health(backend, is_healthy);
            }

            std::this_thread::sleep_for(
                std::chrono::seconds{2});

        }
    }

    Socket create_listening_socket(std::uint16_t port){
        const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);

        if(socket_fd == -1){
            throw_system_error("socket", errno);
        }

        Socket socket{socket_fd};

        const int reuse_address = 1;

        if(::setsockopt(
            socket.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) == -1){
                throw_system_error("setsockopt", errno);
            }

        sockaddr_in server_address{};
        server_address.sin_family = AF_INET;
        server_address.sin_addr.s_addr = htonl(INADDR_ANY); //allow  connection from any ip of the local host
        server_address.sin_port = htons(port); //port of ip

        if(::bind(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&server_address),
            sizeof(server_address)) == -1){
                throw_system_error("bind", errno);
            }
            
        if(::listen(socket.get(), listen_backlog) == -1){
            throw_system_error("listen", errno);
        }

        return socket;

    }

    SendResult send_all(int socket_fd, std::span<const char> data){
        std::size_t total_sent = 0;

        while(total_sent < data.size()){
            const ssize_t bytes_sent = ::send(
            socket_fd,
            data.data() + total_sent,
            data.size() - total_sent,
            0
            );
         
            if(bytes_sent == -1){
                if(errno == EINTR){
                    continue;

                }else{
                    return SendResult{
                        false,
                        errno
                    };

                }
            }

            if(bytes_sent == 0){
                return SendResult{
                    false,
                    0
                };
            }

            total_sent += static_cast<std::size_t>(bytes_sent);
        }

        return SendResult{
            true,
            0
        };

    }

    void print_client_address(const sockaddr_in& client_address){
        std::array<char, INET_ADDRSTRLEN> address_buffer{};

        const char* address = ::inet_ntop(
            AF_INET,
            &client_address.sin_addr,
            address_buffer.data(),
            address_buffer.size()
        );

        if(address == nullptr){
            std::cout << "Client connected from an unknown address\n";
            return;
        }

        std::cout
            << "Client connected from "
            << address_buffer.data()
            << ":"
            << ntohs(client_address.sin_port)
            << '\n';

    }

    BackendConnection connect_to_healthy_backend(){
        for(std::size_t attempts = 0; attempts < backends.size(); attempts++){
            Backend& backend = choose_backend();

            std::optional<Socket> socket = 
                try_connect_to_backend(
                    backend, 
                    backend_connect_timeout);
            
            if(socket.has_value()){
                return BackendConnection{
                    &backend,
                    std::move(*socket)};
            }
            
            //if socket does not have value, this backend is unhealthy
            set_backend_health(backend, false);

        }
        
        throw std::runtime_error("Unable to connect to any healthy backend");

    }

    void forward_data(int source_fd, int destination_fd, Backend& backend, ForwardDirection direction){

        try{
            std::array<char, buffer_size> buffer{};

            while(1){
                const ssize_t bytes_received = ::recv(
                    source_fd,
                    buffer.data(),
                    buffer.size(),
                    0
                );

                if(bytes_received == -1){
                    if(errno == EINTR){
                        continue;
                    }
                    
                    const int errorcode = errno;

                    if(direction == ForwardDirection::backend_to_client){

                        switch(errorcode){
                            case ECONNRESET:
                            case ETIMEDOUT:
                            case ENETRESET:
                                set_backend_health(
                                    backend,
                                    false);
                                break;

                            default: break;
                        }
                    }

                    throw_system_error("recv", errorcode);
                }

                if(bytes_received == 0){
                    if(stop_requested.load(std::memory_order_relaxed)){
                        if(::shutdown(destination_fd, SHUT_RDWR) == -1){
                            throw_system_error("shutdown", errno);
                        }

                        return;
                    }


                    if(::shutdown(destination_fd, SHUT_WR) == -1){
                        throw_system_error("shutdown", errno);
                    }

                    return;
                }

                const SendResult send_result = send_all(
                    destination_fd,
                    std::span<const char>{
                        buffer.data(),
                        static_cast<std::size_t>(bytes_received)
                    }
                );

                if(!send_result.success){
                    if(direction == ForwardDirection::client_to_backend){
                        //sending to backend failed means there is an issue with the backend
                        switch(send_result.errorcode){
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
                        //send() failed with errno error (send() == -1)
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
        }catch (const std::exception& error){
            std::cerr
                << "Forwarding error: "
                << error.what()
                << '\n';

                ::shutdown(source_fd, SHUT_RDWR);
                ::shutdown(destination_fd, SHUT_RDWR);
        }
    }

    void proxy_connection(Socket& client_socket){
        BackendConnection connection = connect_to_healthy_backend();

        Backend& backend = *connection.backend;

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
            ForwardDirection::client_to_backend
        };

        std::thread backend_to_client{
            forward_data,
            connection.socket.get(),
            client_socket.get(),
            std::ref(backend),
            ForwardDirection::backend_to_client
        };

        client_to_backend.join();
        backend_to_client.join();

    }

} //namespace

int main(){
    try{
        struct sigaction action{};
        action.sa_handler = handle_shutdown_signal;

        sigemptyset(&action.sa_mask);

        action.sa_flags = 0;

        if(::sigaction(SIGINT, &action, nullptr) == -1){
            throw_system_error("sigaction(SIGINT)", errno);
        }

        if(::sigaction(SIGTERM, &action, nullptr) == -1){
            throw_system_error("sigaction(SIGTERM)", errno);
        }

        Socket listening_socket = create_listening_socket(server_port);

        std::jthread health_check_thread{
            health_check_loop
        };

        std::vector<std::jthread> connection_threads;

        std::vector<std::weak_ptr<Socket>> active_client_sockets;

        const auto shutdown_server = [&active_client_sockets](){
            stop_requested.store(
                true,
                std::memory_order_relaxed
            );

            for(auto& weak_ptr_socket : active_client_sockets){
                if(auto socket = weak_ptr_socket.lock()){
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
            << server_port
            << "\n";


        try{    
            while(!shutdown_signal_received){
                sockaddr_in client_address{};
                socklen_t client_address_length = sizeof(client_address);

                const int client_fd = ::accept(
                    listening_socket.get(),
                    reinterpret_cast<sockaddr*>(&client_address),
                    &client_address_length
                );

                if(client_fd == -1){
                    if(errno == EINTR){
                        if(shutdown_signal_received){
                            break;
                        }

                        continue;
                    }

                    throw_system_error("accept", errno);
                }

                auto client_socket = std::make_shared<Socket>(client_fd);

                disable_sigpipe(client_socket->get());

                print_client_address(client_address);

                active_client_sockets.emplace_back(client_socket);

                connection_threads.emplace_back(
                    [client_socket](){
                        try{
                            proxy_connection(*client_socket);
                        }catch(const std::exception& error){
                            std::cerr
                                << "Client connection error:"
                                << error.what()
                                << "\n";
                        }

                    }
                );
                
            }
        }catch(...){
            shutdown_server();

            throw;
        }

        shutdown_server();

        std::cout  
            << "Server shutting down\n";

    }catch(const std::exception& error){
        std::cerr
            << "Fatal error: "
            << error.what()
            << '\n';
        return 1;
    }

    return 0;
}