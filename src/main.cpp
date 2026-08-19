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
#include <vector>
#include <atomic>

namespace{

    struct Backend{
        std::string address;
        std::uint16_t port;
    };

    const std::vector<Backend> backends{
        {"127.0.0.1", 9001},
        {"127.0.0.1", 9002},
        {"127.0.0.1", 9003}
    };

    std::atomic<std::size_t> next_backend_index{0};

    const Backend& choose_backend(){
        const std::size_t index = next_backend_index.fetch_add(
            1,
            std::memory_order_relaxed
        );

        return backends[index % backends.size()];
    }

    constexpr std::uint16_t server_port = 8080;
    //constexpr std::uint16_t backend_port = 9000;
    constexpr int listen_backlog = 128; //how many pending connections may queue, waiting for accept()
    constexpr std::size_t buffer_size = 4096;

    [[noreturn]] void throw_system_error(const std::string& operation, int errorcode){
        throw std::runtime_error(
            operation + " failed " + std::strerror(errorcode)
        );
    }

    int create_listening_socket(std::uint16_t port){
        const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);

        if(socket_fd == -1){
            throw_system_error("socket", errno);
        }

        const int reuse_address = 1;

        if(::setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) == -1){
                int errorcode = errno;
                ::close(socket_fd);
                throw_system_error("setsockopt", errorcode);
            }

        sockaddr_in server_address{};
        server_address.sin_family = AF_INET;
        server_address.sin_addr.s_addr = htonl(INADDR_ANY); //allow  connection from any ip of the local host
        server_address.sin_port = htons(port); //port of ip

        if(::bind(
            socket_fd,
            reinterpret_cast<const sockaddr*>(&server_address),
            sizeof(server_address)) == -1){
                int errorcode = errno;
                ::close(socket_fd);
                throw_system_error("bind", errorcode);
            }
            
        if(::listen(socket_fd, listen_backlog) == -1){
            int errorcode = errno;
            ::close(socket_fd);
            throw_system_error("listen", errorcode);
        }

        return socket_fd;

    }

    void send_all(int socket_fd, std::span<const char> data){
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
                    throw_system_error("send", errno);
                }
            }

            total_sent += static_cast<std::size_t>(bytes_sent);

        }

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

    int connect_to_backend(const Backend& backend){
        const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);

        if(socket_fd == -1){
            throw_system_error("socket", errno);
        }

        sockaddr_in backend_address{};
        backend_address.sin_family = AF_INET;
        backend_address.sin_port = htons(backend.port);

        if(::inet_pton(
            AF_INET,
            backend.address.c_str(),
            &backend_address.sin_addr) != 1){
                ::close(socket_fd);
                throw std::runtime_error("inet_pton failed");
        }

        if(::connect(
            socket_fd,
            reinterpret_cast<sockaddr*>(&backend_address),
            sizeof(backend_address)) == -1){
                int errorcode = errno;
                ::close(socket_fd);
                throw_system_error("connect", errorcode);
        }
        
        return socket_fd;

    }

    void forward_data(int source_fd, int destination_fd){

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

                    throw_system_error("recv", errno);
                }

                if(bytes_received == 0){
                    if(::shutdown(destination_fd, SHUT_WR) == -1){
                        throw_system_error("shutdown", errno);
                    }

                    return;
                }

                send_all(
                    destination_fd,
                    std::span<const char>{
                        buffer.data(),
                        static_cast<std::size_t>(bytes_received)
                    }
                );
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

    void proxy_connection(int client_fd){
        const Backend& backend = choose_backend();

        std::cout
            << "Selected backend: "
            << backend.address
            << ':'
            << backend.port
            << '\n';

        const int backend_fd = connect_to_backend(backend);

        std::thread client_to_backend{
            forward_data,
            client_fd,
            backend_fd
        };

        std::thread backend_to_client{
            forward_data,
            backend_fd,
            client_fd
        };

        client_to_backend.join();
        backend_to_client.join();

        ::close(backend_fd);
    }

} //namespace

int main(){
    try{
        const int listening_fd = create_listening_socket(server_port);

        std::cout
            << "Server listening on port "
            << server_port
            << "\n";

        while(1){
            sockaddr_in client_address{};
            socklen_t client_address_length = sizeof(client_address);

            const int client_fd = ::accept(
                listening_fd,
                reinterpret_cast<sockaddr*>(&client_address),
                &client_address_length
            );

            if(client_fd == -1){
                if(errno == EINTR){
                    continue;
                }

                throw_system_error("accept", errno);
            }

            print_client_address(client_address);

            std::thread connection_thread{
                [client_fd](){
                    try{
                        proxy_connection(client_fd);
                    }catch(const std::exception& error){
                        std::cerr
                            << "Client connection error:"
                            << error.what()
                            << "\n";
                    }

                    ::close(client_fd);
                }
            };

            connection_thread.detach();
            
        }
    }catch(const std::exception& error){
        std::cerr
            << "Fatal error: "
            << error.what()
            << '\n';
        return 1;
    }
    
}