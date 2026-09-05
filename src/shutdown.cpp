#include "shutdown.hpp"

#include "errors.hpp"

#include <cerrno>
#include <signal.h>

namespace l4{

    volatile std::sig_atomic_t
        shutdown_signal_received = 0;

    std::atomic<bool>
        stop_requested{false};


    void handle_shutdown_signal(int){
        shutdown_signal_received = 1;
    }


    void install_signal_handlers(){
        struct sigaction action{};

        action.sa_handler =
            handle_shutdown_signal;

        sigemptyset(
            &action.sa_mask
        );

        action.sa_flags = 0;

        if(::sigaction(
            SIGINT,
            &action,
            nullptr
        ) == -1){
            throw_system_error(
                "sigaction(SIGINT)",
                errno
            );
        }

        if(::sigaction(
            SIGTERM,
            &action,
            nullptr
        ) == -1){
            throw_system_error(
                "sigaction(SIGTERM)",
                errno
            );
        }
    }

}