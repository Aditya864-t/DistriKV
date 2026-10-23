#include "socket_util.h"
#include "protocol.h"

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 8001;
    int num_threads = 4;
    int ops_per_thread = 2500;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (arg == "--threads" && i + 1 < argc) num_threads = std::stoi(argv[++i]);
        else if (arg == "--ops" && i + 1 < argc) ops_per_thread = std::stoi(argv[++i]);
    }

    int total_target_ops = num_threads * ops_per_thread * 2; // PUT + GET

    std::cout << "========================================================\n";
    std::cout << " DistriKV Multi-Threaded Berkeley Sockets Stress Benchmark\n";
    std::cout << " Target: " << host << ":" << port << "\n";
    std::cout << " Threads: " << num_threads << " | Ops/Thread: " << ops_per_thread 
              << " | Total Ops: " << total_target_ops << "\n";
    std::cout << "========================================================\n";

    std::atomic<uint64_t> completed_puts{0};
    std::atomic<uint64_t> completed_gets{0};
    std::atomic<uint64_t> failed_ops{0};

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            distrikv::TcpConnection conn;
            if (!conn.connect_to(host, port, 3000)) {
                failed_ops += ops_per_thread * 2;
                return;
            }

            // Phase 1: PUTs
            for (int i = 0; i < ops_per_thread; ++i) {
                std::string k = "t" + std::to_string(t) + "_k_" + std::to_string(i);
                std::string v = "val_" + std::to_string(i);
                auto msg = distrikv::Protocol::make_put_req(k, v);
                if (distrikv::Protocol::send_message(conn.fd(), msg)) {
                    distrikv::Message resp;
                    if (distrikv::Protocol::recv_message(conn.fd(), resp, 2000) && resp.payload == "OK") {
                        completed_puts++;
                    } else {
                        failed_ops++;
                    }
                } else {
                    failed_ops++;
                }
            }

            // Phase 2: GETs
            for (int i = 0; i < ops_per_thread; ++i) {
                std::string k = "t" + std::to_string(t) + "_k_" + std::to_string(i);
                auto msg = distrikv::Protocol::make_get_req(k);
                if (distrikv::Protocol::send_message(conn.fd(), msg)) {
                    distrikv::Message resp;
                    if (distrikv::Protocol::recv_message(conn.fd(), resp, 2000) && resp.payload.find("ERR") == std::string::npos) {
                        completed_gets++;
                    } else {
                        failed_ops++;
                    }
                } else {
                    failed_ops++;
                }
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end_time - start_time;
    double elapsed_sec = duration.count();
    uint64_t total_successful = completed_puts.load() + completed_gets.load();
    double throughput = (elapsed_sec > 0) ? (total_successful / elapsed_sec) : 0;

    std::cout << "\n----------------- Benchmark Finished -----------------\n";
    std::cout << " Elapsed Time:     " << std::fixed << std::setprecision(3) << elapsed_sec << " s\n";
    std::cout << " Completed PUTs:   " << completed_puts.load() << "\n";
    std::cout << " Completed GETs:   " << completed_gets.load() << "\n";
    std::cout << " Failed Ops:       " << failed_ops.load() << "\n";
    std::cout << " Throughput:       \033[1;32m" << std::fixed << std::setprecision(1) << throughput << " ops/sec\033[0m\n";
    std::cout << " Avg Latency:      " << std::fixed << std::setprecision(3) << (elapsed_sec * 1000.0 / total_successful) << " ms/op\n";
    std::cout << "-------------------------------------------------------\n";

    return 0;
}
