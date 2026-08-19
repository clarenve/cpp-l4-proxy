#include <array>
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

namespace{
    //constexpr std::uint16_t port = 9000;
    constexpr int listening_backlog = 128;
    constexpr std::size_t buffer_size = 4096;

    [[noreturn]]void throw_system_error(const std::string& operation, int errorcode){
        throw std::runtime_error(operation + " failed: " + std::strerror(errorcode));
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
            
        if(::listen(socket_fd, listening_backlog) == -1){
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

    void handle_client(int client_fd, std::uint16_t port){
        std::array<char, buffer_size> buffer{};

        while(1){
            const ssize_t bytes_received = ::recv(
                client_fd,
                buffer.data(),
                buffer.size(),
                0
            );

            if(bytes_received == -1){
                if(errno == EINTR){
                    continue;
                }else{
                    throw_system_error("recv", errno);
                }
            }

            if(bytes_received == 0){
                std::cout << "Client disconnected\n";
                return;
            }

            const auto received_size = static_cast<size_t>(bytes_received);

            std::cout << "Backend: "
                << port
                << " received "
                << received_size
                << " bytes\n";

            send_all(
                client_fd,
                std::span<const char>{buffer.data(), received_size}
            );
        }
    }
}

int main(int argc, char* argv[]){

    if(argc != 2){
        std::cerr
            << "Usage: ./backend_server <port>\n";
            return 1;
    }

    try{
        const auto port = static_cast<uint16_t>(std::stoi(argv[1]));

        int listening_fd = create_listening_socket(port);

        std::cout << "Listening on port: " << port << '\n';

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
                }else{
                    throw_system_error("accept", errno);
                }
            }

            std::cout << "Proxy connected to backend\n";

            std::thread client_thread{
                [client_fd, port]{
                    try{
                        handle_client(client_fd, port);
                    }catch(const std::exception& error){
                        std::cerr
                        << "Backend client error: "
                        << error.what()
                        << '\n';
                    }

                    ::close(client_fd);
                }
            };

            client_thread.detach();

        }
    }catch(const std::exception& error){
        std::cerr
            << "Fatal backend error: "
            << error.what()
            << '\n';

            return 1;
    }

}