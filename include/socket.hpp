#pragma once

#include <cstdint>
#include <netinet/in.h>

namespace l4{

    class Socket{
    private:
        static constexpr int invalid_fd = -1;

        int fd_ = invalid_fd;

    public:
        Socket() noexcept = default;

        explicit Socket(int fd) noexcept;

        ~Socket();

        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;

        Socket(Socket&& other) noexcept;
        Socket& operator=(Socket&& other) noexcept;

        int get() const noexcept;

        bool valid() const noexcept;

        int release() noexcept;

        void reset(int new_fd = invalid_fd) noexcept;
    };


    void disable_sigpipe(int socket_fd);

    Socket create_listening_socket(std::uint16_t port);

    void print_client_address(const sockaddr_in& client_address);

    void set_nonblocking(int socket_fd);

}