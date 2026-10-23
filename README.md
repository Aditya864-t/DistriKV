# DistriKV: Distributed Fault-Tolerant Application using C++ Berkeley Sockets

**DistriKV** is a distributed, fault-tolerant, high-performance in-memory key-value database and cluster built from scratch in C++ using POSIX/Berkeley Sockets (`sys/socket.h`, `netinet/in.h`, `arpa/inet.h`).

---

## 🏛️ System Architecture

```
                       +-------------------+
                       |    CLI Client     |
                       | (Berkeley Socket) |
                       +---------+---------+
                                 | TCP Commands (PUT/GET/DEL)
                                 v
+-----------------------------------------------------------------+
| CLUSTER                                                         |
|                                                                 |
|   +------------------+         TCP Replication        +--------------------+ |
|   |   Primary Node   |<------------------------------>|  Secondary Node 1  | |
|   |     (Leader)     |                                |    (Follower)      | |
|   +--------+---------+------------------------------->+--------------------+ |
|            |                         TCP Replication            |             |
|            |                                                    |             |
|            | UDP Heartbeats                                     |             |
|            v                                                    v             |
|   +-------------------------------------------------------------------+       |
|   |                         Secondary Node 2                          |       |
|   |                            (Follower)                             |       |
|   +-------------------------------------------------------------------+       |
+-----------------------------------------------------------------+
                                 | Status API / Sockets
                                 v
                       +-------------------+
                       |   Web Dashboard   |
                       |  (Real-time UI)   |
                       +-------------------+
```

---

## 🚀 Key Features

1. **Native Berkeley Sockets Network Core** (`src/socket_util.{h,cpp}`):
   - Direct POSIX syscalls: `socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`, `recv()`, `setsockopt()`, `select()`, and `fcntl()`.
   - RAII `Socket` wrapper with move-only semantics to prevent descriptor leaks.
   - Non-blocking I/O with timeout enforcement using `select()`.
   - Safe `send_all()` and `recv_all()` loops handling partial sends, socket buffer limits, and `EINTR`/`EAGAIN`.
   - UDP socket abstraction for lightweight datagram peer heartbeats.

2. **Custom Wire Protocol with Framing & CRC32** (`src/protocol.{h,cpp}`):
   - 24-byte binary framing header with Magic (`0x444B5631` / "DKV1"), Version, MsgType, Flags, Term/Epoch, Payload Length, and CRC32 checksum.
   - Message types covering CRUD (`PUT`, `GET`, `DEL`, `KEYS`, `STATS`), Replication (`REPL_APPEND`, `REPL_SYNC`), and Election (`HEARTBEAT`, `VOTE`, `LEADER_ANNOUNCE`).
   - Dual-mode protocol engine supporting both binary wire protocol and interactive line-based ASCII commands (`PUT key val [ttl]`, `GET key`, etc.).

3. **Thread-Safe In-Memory Storage Engine** (`src/storage.{h,cpp}`):
   - Concurrency control via C++17 `std::shared_mutex` (RWLock): concurrent reads (`GET`, `KEYS`, `STATS`) with exclusive write locks (`PUT`, `DEL`).
   - Write-Ahead Log (WAL) tracking term and log index for replication synchronization.
   - Key TTL expiration with passive lazy-eviction on access and active background periodic purging.
   - Atomic snapshot persistence to `dump.kv` using temporary write, fsync, and atomic `rename()`.

4. **Distributed Replication & Dynamic Leader Election** (`src/cluster_node.{h,cpp}`):
   - Dynamic Node Roles: `PRIMARY` (Leader), `SECONDARY` (Follower), `CANDIDATE`.
   - Primary broadcasts writes to all active secondary followers over TCP Berkeley Sockets.
   - Transparent redirection: Secondary nodes reject writes with `MOVED <primary_ip>:<primary_port>`, allowing clients to auto-redirect.
   - UDP heartbeats sent every 500ms by the Primary.
   - Randomized election timeouts (1800ms - 3200ms) prevent split votes when a primary crash is detected.
   - Quorum-based leader election: candidates request votes and transition to Primary upon majority consensus.

5. **Interactive Terminal CLI** (`src/client.cpp`):
   - Colored interactive REPL with auto-redirection and built-in stress benchmarking (`BENCH <ops>`).
   - One-shot execution mode for integration with automated shell scripts.

6. **Real-Time Web Management Dashboard** (`web/` & `src/web_bridge.py`):
   - Built with modern glassmorphism aesthetics, dark obsidian palette, and Google Fonts.
   - Live cluster topology cards displaying node roles, ports, term, and live heartbeat pulses.
   - Interactive Key-Value explorer for search, insertion, and deletion.
   - Chaos failover simulator button ("Kill Leader") and multi-threaded stress benchmark trigger.

---

## 📁 Repository Structure

```
DistriKV1/
├── Makefile                 # Compiles node daemon, client CLI, and benchmark tool
├── README.md                # Comprehensive project documentation
├── bin/                     # Compiled binary executables
├── src/
│   ├── socket_util.h/.cpp   # Berkeley Sockets wrappers & POSIX syscall utilities
│   ├── protocol.h/.cpp      # Binary framing protocol & CRC32 checksums
│   ├── storage.h/.cpp       # In-memory storage, RWLock, TTL, WAL, & snapshots
│   ├── cluster_node.h/.cpp  # Replication, UDP heartbeats, leader election
│   ├── node_main.cpp        # Node daemon executable entry point
│   ├── client.cpp           # Interactive CLI shell
│   ├── benchmark.cpp        # Multi-threaded stress benchmark tool
│   └── web_bridge.py        # Python HTTP & BSD socket bridge server
├── web/
│   ├── index.html           # Glassmorphic web management dashboard
│   ├── style.css            # Dark obsidian theme & micro-animations
│   └── app.js               # Real-time telemetry, KV management, chaos triggers
└── scripts/
    ├── run_cluster.sh       # Launches 3-node cluster + web dashboard
    ├── stop_cluster.sh      # Gracefully shuts down cluster
    └── test_failover.sh     # 12-step automated fault-tolerance test suite
```

---

## 🛠️ Build Instructions

Ensure you have `g++` (C++17 support) and standard POSIX networking libraries installed:

```bash
# Clean and compile all targets
make clean
make
```

Binaries generated in `bin/`:
- `bin/distrikv-node`: Distributed cluster daemon.
- `bin/distrikv-cli`: Interactive terminal client.
- `bin/distrikv-bench`: Standalone multi-threaded stress test tool.

---

## 🚦 Running the Cluster

### Start Cluster (1 Primary + 2 Secondaries + Web Dashboard)
```bash
bash scripts/run_cluster.sh
```

Nodes launched:
- **Node 1**: TCP `8001`, UDP Heartbeat `9001` (Initial PRIMARY)
- **Node 2**: TCP `8002`, UDP Heartbeat `9002` (SECONDARY)
- **Node 3**: TCP `8003`, UDP Heartbeat `9003` (SECONDARY)
- **Web Dashboard**: `http://127.0.0.1:8080`

### Stop Cluster
```bash
bash scripts/stop_cluster.sh
```

---

## 💻 Using the Interactive CLI Client

Start the interactive terminal shell connected to Node 1:
```bash
./bin/distrikv-cli --host 127.0.0.1 --port 8001
```

### CLI Commands:
```text
distrikv [127.0.0.1:8001]> PUT user:1001 "Alice" 60
OK
distrikv [127.0.0.1:8001]> GET user:1001
"Alice"
distrikv [127.0.0.1:8001]> KEYS *
user:1001
distrikv [127.0.0.1:8001]> STATS
Node ID: 1
Role: PRIMARY
Term: 0
Total Keys: 1
...
distrikv [127.0.0.1:8001]> BENCH 1000
----------------- Benchmark Results -----------------
 Throughput:       11349.3 ops/sec
 Avg Latency:      0.088 ms/op
-----------------------------------------------------
distrikv [127.0.0.1:8001]> QUIT
```

---

## 🧪 Automated Fault-Tolerance Test Suite

DistriKV includes an end-to-end test suite (`scripts/test_failover.sh`) that verifies:
1. Cluster bootstrapping (Primary on 8001, Secondaries on 8002, 8003).
2. Primary CRUD operations.
3. State replication across secondaries.
4. Automatic write redirection from secondary to primary.
5. Primary crash simulation (`kill -9`).
6. Follower timeout detection via UDP heartbeats and dynamic election.
7. Zero data loss on newly elected primary.
8. Revived node catch-up and log synchronization.

Run the test suite:
```bash
bash scripts/test_failover.sh
```

Expected output:
```text
==========================================================
 Tests Completed: 12
 Tests Passed:    12 / 12
 ALL DISTRIBUTED FAULT-TOLERANCE TESTS PASSED!
==========================================================
```

---

## 🌐 Web Dashboard Preview

Open `http://127.0.0.1:8080` in your web browser:
- **Live Topology**: Real-time status cards for each node showing role, port, and pulsing heartbeat indicators.
- **Key-Value Studio**: View stored data, filter keys, insert records, and delete entries.
- **Chaos Controls**: Simulate leader failure with one click and observe election transitions in the live log.
- **Terminal**: Execute raw database commands right from the browser.
