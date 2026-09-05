#include "socket.hpp"

#include "errors.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace l4{

    namespace{

        constexpr int listen_backlog = 128;

    }


    //Socket RAII

    Socket::Socket(int fd) noexcept
        : fd_{fd}
    {}


    Socket::~Socket(){
        reset();
    }


    Socket::Socket(Socket&& other) noexcept
        : fd_{other.fd_}
    {
        other.fd_ = invalid_fd;
    }


    Socket& Socket::operator=(Socket&& other) noexcept{
        if(this != &other){
            reset();

            fd_ = other.fd_;
            other.fd_ = invalid_fd;
        }

        return *this;
    }


    int Socket::get() const noexcept{
        return fd_;
    }


    bool Socket::valid() const noexcept{
        return fd_ != invalid_fd;
    }


    int Socket::release() noexcept{
        const int fd = fd_;

        fd_ = invalid_fd;

        return fd;
    }


    void Socket::reset(int new_fd) noexcept{
        if(fd_ != invalid_fd && fd_ != new_fd){
            ::close(fd_);
        }

        fd_ = new_fd;
    }


    // SOCKET UTILS

    void disable_sigpipe(int socket_fd){
        const int value = 1;

        if(::setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_NOSIGPIPE,
            &value,
            sizeof(value)
        ) == -1){
            throw_system_error(
                "setsockopt(SO_NOSIGPIPE)",
                errno
            );
        }
    }


    Socket create_listening_socket(
        std::uint16_t port
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

        const int reuse_address = 1;

        if(::setsockopt(
            socket.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)
        ) == -1){
            throw_system_error(
                "setsockopt",
                errno
            );
        }

        sockaddr_in server_address{};

        server_address.sin_family = AF_INET;
        server_address.sin_addr.s_addr =
            htonl(INADDR_ANY);
        server_address.sin_port =
            htons(port);

        if(::bind(
            socket.get(),
            reinterpret_cast<const sockaddr*>(
                &server_address
            ),
            sizeof(server_address)
        ) == -1){
            throw_system_error(
                "bind",
                errno
            );
        }

        if(::listen(
            socket.get(),
            listen_backlog
        ) == -1){
            throw_system_error(
                "listen",
                errno
            );
        }

        return socket;
    }


    void print_client_address(
        const sockaddr_in& client_address
    ){
        std::array<char, INET_ADDRSTRLEN>
            address_buffer{};

        const char* address =
            ::inet_ntop(
                AF_INET,
                &client_address.sin_addr,
                address_buffer.data(),
                address_buffer.size()
            );

        if(address == nullptr){
            std::cout
                << "Client connected from "
                "an unknown address\n";

            return;
        }

        std::cout
            << "Client connected from "
            << address_buffer.data()
            << ':'
            << ntohs(client_address.sin_port)
            << '\n';
    }

}