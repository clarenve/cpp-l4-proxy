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

500 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 500 10000

Benchmark results
-----------------
Connections:        500
Warmup/client:       100
Requests/client:     10000
Completed requests:  5000000
Failed requests:     0
Total time:          85.0699 s
Throughput:          58775.2 req/s

Latency
-------
Average: 8367.85 us
p50:     5899.62 us
p95:     20070.8 us
p99:     102273 us

Non-blocking kqueue imlpementation results
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
Total time:          0.411798 s
Throughput:          24283.7 req/s

Latency
-------
Average: 41.1792 us
p50:     40 us
p95:     50.208 us
p99:     62.1258 us

10 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 10 10000

Benchmark results
-----------------
Connections:        10
Warmup/client:       100
Requests/client:     10000
Completed requests:  100000
Failed requests:     0
Total time:          1.24498 s
Throughput:          80322.6 req/s

Latency
-------
Average: 124.997 us
p50:     121.209 us
p95:     177.25 us
p99:     207.959 us

25 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 25 10000

Benchmark results
-----------------
Connections:        25
Warmup/client:       100
Requests/client:     10000
Completed requests:  250000
Failed requests:     0
Total time:          2.95852 s
Throughput:          84501.7 req/s

Latency
-------
Average: 296.651 us
p50:     291.042 us
p95:     367.875 us
p99:     418.958 us

50 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 50 10000

Benchmark results
-----------------
Connections:        50
Warmup/client:       100
Requests/client:     10000
Completed requests:  500000
Failed requests:     0
Total time:          5.97699 s
Throughput:          83654.2 req/s

Latency
-------
Average: 599.838 us
p50:     575.625 us
p95:     752.208 us
p99:     939.708 us

100 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 100 10000

Benchmark results
-----------------
Connections:        100
Warmup/client:       100
Requests/client:     10000
Completed requests:  1000000
Failed requests:     0
Total time:          11.9207 s
Throughput:          83887.9 req/s

Latency
-------
Average: 1192.09 us
p50:     1168.75 us
p95:     1352.25 us
p99:     1726.79 us

250 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 250 10000

Benchmark results
-----------------
Connections:        250
Warmup/client:       100
Requests/client:     10000
Completed requests:  2500000
Failed requests:     0
Total time:          30.0974 s
Throughput:          83063.6 req/s

Latency
-------
Average: 3009.7 us
p50:     2965.17 us
p95:     3293.62 us
p99:     3938.5 us

500 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 500 10000

Benchmark results
-----------------
Connections:        500
Warmup/client:       100
Requests/client:     10000
Completed requests:  5000000
Failed requests:     0
Total time:          60.807 s
Throughput:          82227.4 req/s

Latency
-------
Average: 6079.01 us
p50:     6031.83 us
p95:     6643.92 us
p99:     9072.13 us

750 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 750 10000

Benchmark results
-----------------
Connections:        750
Warmup/client:       100
Requests/client:     10000
Completed requests:  7500000
Failed requests:     0
Total time:          88.4682 s
Throughput:          84776.2 req/s

Latency
-------
Average: 8844.28 us
p50:     8890.58 us
p95:     11328.5 us
p99:     12131 us

1000 CONCURRENT CONNECTIONS
./load_generator 127.0.0.1 8080 1000 10000

Benchmark results
-----------------
Connections:        1000
Warmup/client:       100
Requests/client:     10000
Completed requests:  10000000
Failed requests:     0
Total time:          130.223 s
Throughput:          76791.3 req/s

Latency
-------
Average: 13019.4 us
p50:     12962.3 us
p95:     13739 us
p99:     14979.6 us

## Benchmark Summary

The blocking thread-per-connection implementation achieved better performance at low-to-moderate concurrency. It reached a peak throughput of approximately **93k requests/s at 50 concurrent connections**, while the single-threaded kqueue implementation plateaued at approximately **82–85k requests/s** across most tested concurrency levels.

This is consistent with the additional event-management overhead introduced by the non-blocking architecture. For small localhost requests and relatively few connections, the blocking implementation follows a simpler execution path and can make effective use of multiple threads and CPU cores.

However, the difference changes significantly as concurrency increases.

At **250 concurrent connections**, the blocking implementation's throughput dropped from approximately 92k requests/s at 100 connections to **70.8k requests/s**, while its p99 latency increased sharply to **26.5 ms**. The kqueue implementation maintained approximately **83.1k requests/s**, with a p99 latency of only **3.94 ms**.

At **500 concurrent connections**, the difference became more pronounced. At this concurrency level, the kqueue implementation achieved approximately **40% higher throughput**, **27% lower average latency**, **67% lower p95 latency**, and **91% lower p99 latency**.

An important observation is that the blocking implementation retained a competitive median latency even as its tail latency deteriorated significantly. At 500 connections, its p50 was approximately 5.9 ms, but its p99 exceeded 100 ms. This suggests that many requests continued to complete quickly while a smaller proportion experienced substantial delays, likely due to increasing thread scheduling and contention overhead.

The kqueue implementation showed a much tighter latency distribution. Between 250 and 750 concurrent connections, throughput remained approximately constant at **82–85k requests/s**:

```text
Connections    Throughput

250            83.1k req/s
500            82.2k req/s
750            84.8k req/s
```

Latency increased as more concurrent clients shared roughly the same processing capacity, but tail latency remained comparatively controlled.

At **1000 concurrent connections**, throughput decreased to **76.8k requests/s**, while average latency increased to approximately **13.0 ms**. The latency distribution remained relatively tight:

```text
p50:  12.96 ms
p95:  13.74 ms
p99:  14.98 ms
```

This differs significantly from the blocking implementation's high-concurrency behavior, where p95 and p99 latency diverged sharply from the median.

CPU observation during the 1000-connection test showed the single-threaded `proxy_kqueue` process using approximately one logical CPU core at close to full utilization. This suggests that the current implementation is approaching the processing capacity of a single event loop rather than suffering from the thread-scheduling instability observed in the blocking implementation.

The benchmark therefore demonstrates the expected architectural tradeoff:

```text
Blocking thread-per-connection

Advantages:
- lower overhead at low concurrency
- higher peak throughput at modest concurrency
- can naturally consume multiple CPU cores through many threads

Disadvantages:
- throughput degrades as connection count grows
- significantly worse high-concurrency tail latency
- increasing scheduler and thread-management overhead


Single-threaded kqueue

Advantages:
- stable throughput across a much wider concurrency range
- significantly better p95/p99 behavior under high concurrency
- large numbers of connections managed by one event loop
- predictable per-connection ownership and memory management

Disadvantages:
- additional event-management overhead at low concurrency
- one event loop eventually becomes limited by one CPU core
- further single-host scaling would require multiple event-loop workers
  or multiple proxy instances
```

Overall, the results do not show that non-blocking event-driven I/O is universally faster. Instead, they demonstrate its primary benefit: **better scalability and more predictable latency as the number of concurrent connections increases**.

The blocking implementation is faster under lighter workloads, but begins to degrade noticeably between 100 and 250 concurrent connections and shows severe tail-latency degradation by 500 connections. The kqueue implementation sacrifices some low-concurrency performance in exchange for maintaining approximately constant throughput and substantially more stable latency under high concurrency.

### Benchmark Limitations

These tests were performed locally using persistent TCP connections and small request/response messages. The load generator also uses one `std::thread` per client connection. At very high concurrency, particularly around 1000 connections, the load generator itself introduces additional scheduler and CPU overhead.

Therefore, results beyond this point should not be interpreted as measuring the proxy in complete isolation. A future event-driven load generator or distributed load generation setup would be required to accurately test substantially higher connection counts.

The current results are sufficient to demonstrate the scalability difference between the blocking and single-event-loop architectures and establish a baseline for future Linux `epoll`, multi-worker, and horizontally scaled proxy implementations.