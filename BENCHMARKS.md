This document records the performance of the C++ Layer 4 TCP proxy across different implementation stages. Each test was performed three times, and the median result was recorded.

Baseline results below are intended to serve as a reference for later comparisons against non-blocking kqueue/epoll based architecture.

Blocking thread-per-connection results
======================================

1 CONCURRENT CONNECTION
./load_generator 127.0.0.1 8080 1 10000

Benchmark results
-----------------
Connections:        1
Warmup/client:       100
Requests/client:     10000
Completed requests:  10000
Failed requests:     0
Total time:          0.354388 s
Throughput:          28217.7 req/s

Latency
-------
Average: 35.6998 us
p50:     34.208 us
p95:     44.625 us
p99:     66.2276 us

10 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 10 10000

Benchmark results
-----------------
Connections:        10
Warmup/client:       100
Requests/client:     10000
Completed requests:  100000
Failed requests:     0
Total time:          1.1927 s
Throughput:          83843.3 req/s

Latency
-------
Average: 116.994 us
p50:     114.875 us
p95:     159.042 us
p99:     184.708 us

25 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 25 10000

Benchmark results
-----------------
Connections:        25
Warmup/client:       100
Requests/client:     10000
Completed requests:  250000
Failed requests:     0
Total time:          2.73035 s
Throughput:          91563.3 req/s

Latency
-------
Average: 273.124 us
p50:     277.209 us
p95:     330.417 us
p99:     376.125 us

50 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 50 10000

Benchmark results
-----------------
Connections:        50
Warmup/client:       100
Requests/client:     10000
Completed requests:  500000
Failed requests:     0
Total time:          5.37517 s
Throughput:          93020.4 req/s

Latency
-------
Average: 535.608 us
p50:     539.625 us
p95:     637.166 us
p99:     925 us

100 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 100 10000

Benchmark results
-----------------
Connections:        100
Warmup/client:       100
Requests/client:     10000
Completed requests:  1000000
Failed requests:     0
Total time:          10.8993 s
Throughput:          91748.7 req/s

Latency
-------
Average: 1080.97 us
p50:     1050.83 us
p95:     1608.33 us
p99:     2425 us

250 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 250 10000

Benchmark results
-----------------
Connections:        250
Warmup/client:       100
Requests/client:     10000
Completed requests:  2500000
Failed requests:     0
Total time:          35.2997 s
Throughput:          70822.1 req/s

Latency
-------
Average: 3438.24 us
p50:     2038.71 us
p95:     7841.75 us
p99:     26465 us