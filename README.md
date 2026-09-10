# Multi-Threaded Adaptive HTTP/HTTPS Proxy Server

A high-performance, low-level network proxy server implemented in C. Features a POSIX thread pool architecture, transparent HTTP/HTTPS CONNECT tunneling, adaptive dual-policy caching (LRU/LFU), disk-backed persistence, speculative HTML prefetching, host blacklisting, and real-time operational telemetry.

---

## **Key Features**

* **Multi-Threaded Concurrency Engine**
* Fixed-size 16-worker thread pool built on POSIX pthreads.
* Thread-safe producer-consumer task queue utilizing `pthread_mutex_t` and `pthread_cond_t`.
* Isolated task queue routing to decouple active client traffic from speculative background prefetch operations.


* **Protocol & Network Proxying**
* Robust HTTP request parsing (extracts host, port, path, headers, and methods).
* Direct HTTP GET request proxying with origin forwarding and response caching.
* Transparent HTTPS proxying via the `HTTP CONNECT` method, establishing bidirectional TCP tunnels using `select()` multiplexing.


* **Adaptive Multi-Level Caching**
* Dual-layer caching topology: **In-Memory Cache $\rightarrow$ Persistent Disk Cache $\rightarrow$ Origin Server**.
* Concurrent implementation of **Least Recently Used (LRU)** and **Least Frequently Used (LFU)** eviction models.
* **Ghost Cache Controller:** Tracks virtual hit/miss patterns of an alternate policy alongside the active policy.
* Dynamic, runtime policy switching executed when the ghost policy demonstrates a **$\ge 10\%$ relative hit-rate performance advantage**, preventing oscillation.



```
                  Client Request
                        │
                        ▼
             ┌─────────────────────┐
             │   In-Memory Cache   │
             │   Active LRU/LFU    │
             └──────────┬──────────┘
                        │ MISS
                        ▼
             ┌─────────────────────┐
             │  Disk-Backed Cache  │
             └──────────┬──────────┘
                        │ MISS
                        ▼
             ┌─────────────────────┐
             │    Origin Server    │
             └──────────┬──────────┘
                        │
                        ▼
                  Store Response
                 (Memory + Disk)

```

* **Speculative Resource Prefetching**
* Non-blocking response parser inspects cached HTML content for linked sub-resources (`<img>`, `<link rel="stylesheet">`, `<script>`).
* Asynchronously dispatches background prefetch jobs into the task pool to warm the cache ahead of subsequent client requests.


* **Real-Time Telemetry & Monitoring**
* Dedicated background monitoring thread collecting operational metrics over rolling 60-second windows.
* Tracks throughput, absolute request volume, cache hits/misses, and effective hit-rate ratios written to system logs.


* **Access Control & Configuration**
* Host-based blacklist engine rejecting unauthorized requests with `403 Forbidden` statuses prior to socket creation.
* Externalized configuration via `proxy.conf` controlling thread counts, memory bounds, and item limits.



---

## **System Architecture**

```
                       ┌─────────────────┐
                       │   Client(s)     │
                       └────────┬────────┘
                                │
                                ▼
                       ┌─────────────────┐
                       │  TCP Listener   │
                       └────────┬────────┘
                                │
                                ▼
                       ┌─────────────────┐
                       │   Task Queue    │
                       │  Mutex + Cond.  │
                       └────────┬────────┘
                                │
               ┌────────────────┼────────────────┐
               ▼                ▼                ▼
          Worker 1          Worker 2          Worker N
               │                │                │
               └────────────────┼────────────────┘
                                │
                       ┌────────┴─────────┐
                       │ Request Handler  │
                       └────────┬─────────┘
                                │
               ┌────────────────┼─────────────────┐
               ▼                ▼                 ▼
         HTTP GET          HTTPS CONNECT       Blacklist
               │                │
               ▼                ▼
        Cache Lookup       TCP Tunnel
               │
        ┌──────┴───────┐
        ▼              ▼
     Memory          Disk
     LRU/LFU         Cache
        │              │
        └──────┬───────┘
               │ MISS
               ▼
         Origin Server
               │
               ▼
       Cache + Prefetch

```

---

## **Configuration**

Server parameters are managed via `proxy.conf` at launch:

```ini
port = 8888
threads = 16
cache_size_mb = 250
element_size_mb = 5

```

---

## **Build & Usage**

### **Prerequisites**

* GCC Compiler (`c99` or later)
* POSIX Threads library (`pthread`)
* OpenSSL development headers (`libssl-dev`)
* Linux / POSIX-compliant OS environment

### **Compilation**

Build the proxy server and test client using the provided `Makefile`:

```bash
make

```

To clear build artifacts:

```bash
make clean

```

### **Running the Server**

Start the proxy instance:

```bash
./proxy_server

```

### **Testing Connections**

Use the compiled test client to evaluate HTTP and HTTPS execution paths:

* **HTTP GET Routing:**
```bash
./test_client localhost 8888 http://example.com

```


* **HTTPS Tunneling (CONNECT):**
```bash
./test_client localhost 8888 https://www.google.com

```



---

## **Tech Stack**

* **Language:** C (POSIX Systems Programming)
* **Networking:** Sockets (`AF_INET`), TCP/IP, HTTP/1.1, HTTP CONNECT Method, `select()` I/O Multiplexing
* **Concurrency:** POSIX Threads (`pthreads`), Mutex Locks (`pthread_mutex_t`), Condition Variables (`pthread_cond_t`), Producer-Consumer Queues
* **Data Structures & Caching:** Double-Linked Lists, Hash Maps, Adaptive LRU/LFU Eviction, Ghost Caches, Persistent Disk I/O
