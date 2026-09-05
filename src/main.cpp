#include "server.hpp"
#include "shutdown.hpp"

#include <cstdint>
#include <exception>
#include <iostream>

namespace{

    constexpr std::uint16_t server_port = 8080;

}

int main(){
    try{
        l4::install_signal_handlers();

        l4::run_server(
            server_port
        );
    }catch(const std::exception& error){
        std::cerr
            << "Fatal error: "
            << error.what()
            << '\n';

        return 1;
    }

    return 0;
}