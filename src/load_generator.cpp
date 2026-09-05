#include <arpa/inet.h>
#include <algorithm>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t warmup_requests = 100;

struct WorkerResult {
    std::vector<double> latencies_us;
    std::size_t completed_requests = 0;
    std::size_t failed_requests = 0;
};

void throw_system_error(
    const std::string& operation,
    int errorcode
) {
    throw std::runtime_error(
        operation + " failed: " +
        std::strerror(errorcode)
    );
}

void send_all(
    int socket_fd,
    std::span<const char> data
) {
    std::size_t total_sent = 0;

    while(total_sent < data.size()) {
        const ssize_t bytes_sent = ::send(
            socket_fd,
            data.data() + total_sent,
            data.size() - total_sent,
            0
        );

        if(bytes_sent == -1) {
            if(errno == EINTR) {
                continue;
            }

            throw_system_error(
                "send",
                errno
            );
        }

        if(bytes_sent == 0) {
            throw std::runtime_error(
                "send made no progress"
            );
        }

        total_sent +=
            static_cast<std::size_t>(
                bytes_sent
            );
    }
}

void recv_exact(
    int socket_fd,
    std::span<char> buffer
) {
    std::size_t total_received = 0;

    while(total_received < buffer.size()) {
        const ssize_t bytes_received = ::recv(
            socket_fd,
            buffer.data() + total_received,
            buffer.size() - total_received,
            0
        );

        if(bytes_received == -1) {
            if(errno == EINTR) {
                continue;
            }

            throw_system_error(
                "recv",
                errno
            );
        }

        if(bytes_received == 0) {
            throw std::runtime_error(
                "peer closed connection before full response"
            );
        }

        total_received +=
            static_cast<std::size_t>(
                bytes_received
            );
    }
}

int connect_to_proxy(
    const std::string& host,
    std::uint16_t port
) {
    const int socket_fd =
        ::socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if(socket_fd == -1) {
        throw_system_error(
            "socket",
            errno
        );
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    const int address_result =
        ::inet_pton(
            AF_INET,
            host.c_str(),
            &address.sin_addr
        );

    if(address_result == 0) {
        ::close(socket_fd);

        throw std::runtime_error(
            "invalid IPv4 address: " +
            host
        );
    }

    if(address_result == -1) {
        const int errorcode = errno;

        ::close(socket_fd);

        throw_system_error(
            "inet_pton",
            errorcode
        );
    }

    if(::connect(
            socket_fd,
            reinterpret_cast<sockaddr*>(
                &address
            ),
            sizeof(address)
        ) == -1) {

        const int errorcode = errno;

        ::close(socket_fd);

        throw_system_error(
            "connect",
            errorcode
        );
    }

    return socket_fd;
}

void perform_request(
    int socket_fd,
    std::string_view message,
    std::vector<char>& response
) {
    send_all(
        socket_fd,
        std::span<const char>{
            message.data(),
            message.size()
        }
    );

    recv_exact(
        socket_fd,
        std::span<char>{
            response.data(),
            response.size()
        }
    );

    if(!std::equal(
            response.begin(),
            response.end(),
            message.begin(),
            message.end()
        )) {

        throw std::runtime_error(
            "response did not match request"
        );
    }
}

WorkerResult run_worker(
    const std::string& host,
    std::uint16_t port,
    std::size_t requests_per_client,
    std::barrier<>& start_barrier
) {
    WorkerResult result;

    result.latencies_us.reserve(
        requests_per_client
    );

    int socket_fd = -1;

    try {
        socket_fd =
            connect_to_proxy(
                host,
                port
            );

        constexpr std::string_view message =
            "ping";

        std::vector<char> response(
            message.size()
        );

        // Warm-up requests are not measured.
        for(
            std::size_t i = 0;
            i < warmup_requests;
            ++i
        ) {
            perform_request(
                socket_fd,
                message,
                response
            );
        }

        // Worker is now connected and warmed up.
        // Wait for every worker + main.
        start_barrier.arrive_and_wait();

        for(
            std::size_t request_index = 0;
            request_index < requests_per_client;
            ++request_index
        ) {
            try {
                const auto start =
                    Clock::now();

                perform_request(
                    socket_fd,
                    message,
                    response
                );

                const auto end =
                    Clock::now();

                const double latency_us =
                    std::chrono::duration<
                        double,
                        std::micro
                    >(
                        end - start
                    ).count();

                result.latencies_us.push_back(
                    latency_us
                );

                ++result.completed_requests;
            }catch(...) {
                ++result.failed_requests;

                // Stop using this persistent TCP
                // connection after a request failure.
                break;
            }
        }
    }catch(...) {
        result.failed_requests +=
            requests_per_client;
    }

    if(socket_fd != -1) {
        ::close(socket_fd);
    }

    return result;
}

double percentile(
    const std::vector<double>& sorted_values,
    double percentile_value
) {
    if(sorted_values.empty()) {
        return 0.0;
    }

    const double position =
        percentile_value *
        static_cast<double>(
            sorted_values.size() - 1
        );

    const std::size_t lower_index =
        static_cast<std::size_t>(
            std::floor(position)
        );

    const std::size_t upper_index =
        static_cast<std::size_t>(
            std::ceil(position)
        );

    if(lower_index == upper_index) {
        return sorted_values[lower_index];
    }

    const double fraction =
        position -
        static_cast<double>(
            lower_index
        );

    return
        sorted_values[lower_index] *
            (1.0 - fraction)
        +
        sorted_values[upper_index] *
            fraction;
}

} // namespace

int main(
    int argc,
    char* argv[]
) {
    if(argc != 5) {
        std::cerr
            << "Usage: "
            << argv[0]
            << " <host> <port> "
            << "<connections> "
            << "<requests_per_client>\n";

        return 1;
    }

    try {
        const std::string host =
            argv[1];

        const std::uint16_t port =
            static_cast<std::uint16_t>(
                std::stoul(argv[2])
            );

        const std::size_t connection_count =
            std::stoull(argv[3]);

        const std::size_t requests_per_client =
            std::stoull(argv[4]);

        std::vector<std::thread> workers;

        workers.reserve(
            connection_count
        );

        std::vector<WorkerResult> results(
            connection_count
        );

        // CHANGED:
        // Every worker participates, plus main.
        std::barrier start_barrier{
            static_cast<std::ptrdiff_t>(
                connection_count + 1
            )
        };

        for(
            std::size_t i = 0;
            i < connection_count;
            ++i
        ) {
            workers.emplace_back(
                [&, i]() {
                    results[i] =
                        run_worker(
                            host,
                            port,
                            requests_per_client,
                            start_barrier
                        );
                }
            );
        }

        // CHANGED:
        //
        // Main waits here while workers:
        // - establish TCP connections
        // - perform warm-up requests
        // - reach their barrier
        //
        // Once every worker and main arrive,
        // everyone is released.
        start_barrier.arrive_and_wait();

        // CHANGED:
        // Start timing only after setup + warm-up.
        const auto benchmark_start =
            Clock::now();

        for(std::thread& worker : workers) {
            worker.join();
        }

        const auto benchmark_end =
            Clock::now();

        std::vector<double> all_latencies;

        std::size_t completed_requests = 0;
        std::size_t failed_requests = 0;

        for(const WorkerResult& result : results) {
            completed_requests +=
                result.completed_requests;

            failed_requests +=
                result.failed_requests;

            all_latencies.insert(
                all_latencies.end(),
                result.latencies_us.begin(),
                result.latencies_us.end()
            );
        }

        std::sort(
            all_latencies.begin(),
            all_latencies.end()
        );

        const double total_seconds =
            std::chrono::duration<double>(
                benchmark_end -
                benchmark_start
            ).count();

        const double throughput =
            total_seconds > 0.0
                ? static_cast<double>(
                    completed_requests
                ) / total_seconds
                : 0.0;

        double average_latency = 0.0;

        if(!all_latencies.empty()) {
            average_latency =
                std::accumulate(
                    all_latencies.begin(),
                    all_latencies.end(),
                    0.0
                )
                /
                static_cast<double>(
                    all_latencies.size()
                );
        }

        std::cout
            << "\nBenchmark results\n"
            << "-----------------\n"
            << "Connections:        "
            << connection_count
            << '\n'
            << "Warmup/client:       "
            << warmup_requests
            << '\n'
            << "Requests/client:     "
            << requests_per_client
            << '\n'
            << "Completed requests:  "
            << completed_requests
            << '\n'
            << "Failed requests:     "
            << failed_requests
            << '\n'
            << "Total time:          "
            << total_seconds
            << " s\n"
            << "Throughput:          "
            << throughput
            << " req/s\n";

        if(!all_latencies.empty()) {
            std::cout
                << "\nLatency\n"
                << "-------\n"
                << "Average: "
                << average_latency
                << " us\n"
                << "p50:     "
                << percentile(
                    all_latencies,
                    0.50
                )
                << " us\n"
                << "p95:     "
                << percentile(
                    all_latencies,
                    0.95
                )
                << " us\n"
                << "p99:     "
                << percentile(
                    all_latencies,
                    0.99
                )
                << " us\n";
        }
    }catch(const std::exception& error) {
        std::cerr
            << "Fatal error: "
            << error.what()
            << '\n';

        return 1;
    }

    return 0;
}