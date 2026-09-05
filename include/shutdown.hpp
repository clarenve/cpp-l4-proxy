#pragma once

#include <atomic>
#include <csignal>

namespace l4{

    extern volatile std::sig_atomic_t shutdown_signal_received;

    extern std::atomic<bool> stop_requested;


    void handle_shutdown_signal(int);

    void install_signal_handlers();

}