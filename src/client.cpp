#include "socket_util.h"
#include "protocol.h"

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <chrono>
#include <iomanip>

namespace distrikv {

class DistriKvClient {
    bool is_interactive_ = false;

public:
    DistriKvClient(std::string host, int port, bool interactive = false)
        : is_interactive_(interactive), current_host_(std::move(host)), current_port_(port) {}

    bool connect(int timeout_ms = 3000) {
        conn_.close();
        if (!conn_.connect_to(current_host_, current_port_, timeout_ms)) {
            if (is_interactive_) {
                std::cerr << "\033[1;31m[Client] Connection failed to " << current_host_ << ":" << current_port_ << "\033[0m\n";
            } else {
                std::cerr << "[Client] Connection failed to " << current_host_ << ":" << current_port_ << "\n";
            }
            return false;
        }
        return true;
    }

    bool execute_command(const std::string& line, bool auto_redirect = true) {
        auto tokens = Protocol::tokenize(line);
        if (tokens.empty()) return true;

        std::string cmd = tokens[0];
        for (auto& c : cmd) c = toupper(c);

        if (cmd == "CONNECT" && tokens.size() >= 3) {
            current_host_ = tokens[1];
            current_port_ = std::stoi(tokens[2]);
            if (connect()) {
                if (is_interactive_) {
                    std::cout << "\033[1;32mConnected to " << current_host_ << ":" << current_port_ << "\033[0m\n";
                } else {
                    std::cout << "Connected to " << current_host_ << ":" << current_port_ << "\n";
                }
            }
            return true;
        }

        if (cmd == "BENCH") {
            int ops = 1000;
            if (tokens.size() >= 2) {
                try { ops = std::stoi(tokens[1]); } catch (...) {}
            }
            run_benchmark(ops);
            return true;
        }

        if (cmd == "HELP") {
            print_help();
            return true;
        }

        if (cmd == "QUIT" || cmd == "EXIT") {
            return false;
        }

        // Build Protocol Message
        Message req;
        if (cmd == "PUT" && tokens.size() >= 3) {
            int64_t ttl = (tokens.size() >= 4) ? std::stoll(tokens[3]) : 0;
            req = Protocol::make_put_req(tokens[1], tokens[2], ttl);
        } else if (cmd == "GET" && tokens.size() >= 2) {
            req = Protocol::make_get_req(tokens[1]);
        } else if (cmd == "DEL" && tokens.size() >= 2) {
            req = Protocol::make_del_req(tokens[1]);
        } else if (cmd == "KEYS") {
            req = Protocol::make_keys_req(tokens.size() >= 2 ? tokens[1] : "*");
        } else if (cmd == "STATS") {
            req = Protocol::make_stats_req();
        } else if (cmd == "CLUSTER") {
            req = Protocol::make_cluster_info_req();
        } else if (cmd == "PING") {
            req = Message(MsgType::PING, "PING");
        } else {
            if (is_interactive_) {
                std::cout << "\033[1;33mUnknown command. Type HELP for available commands.\033[0m\n";
            } else {
                std::cout << "ERR Unknown command\n";
            }
            return true;
        }

        if (!conn_.is_valid()) {
            if (!connect()) return true;
        }

        if (!Protocol::send_message(conn_.fd(), req)) {
            if (is_interactive_) {
                std::cerr << "\033[1;31mSend failed. Reconnecting...\033[0m\n";
            }
            if (!connect() || !Protocol::send_message(conn_.fd(), req)) {
                if (is_interactive_) {
                    std::cerr << "\033[1;31mServer unreachable.\033[0m\n";
                }
                return true;
            }
        }

        Message resp;
        if (!Protocol::recv_message(conn_.fd(), resp, 5000)) {
            if (is_interactive_) {
                std::cerr << "\033[1;31mReceive timeout or server disconnected.\033[0m\n";
            }
            conn_.close();
            return true;
        }

        // Check for redirection: "MOVED host:port"
        if (resp.msg_type == MsgType::REDIRECT_RESP) {
            std::string target = resp.payload;
            if (target.rfind("MOVED ", 0) == 0) {
                target = target.substr(6);
            }
            if (is_interactive_) {
                std::cout << "\033[1;33m[Cluster Redirect] Write rejected by secondary node. Primary leader is at " 
                          << target << "\033[0m\n";
            }

            if (auto_redirect) {
                auto colon = target.find(':');
                if (colon != std::string::npos) {
                    current_host_ = target.substr(0, colon);
                    current_port_ = std::stoi(target.substr(colon + 1));
                    if (is_interactive_) {
                        std::cout << "\033[1;36mAuto-redirecting connection to " << current_host_ << ":" << current_port_ << "...\033[0m\n";
                    }
                    if (connect()) {
                        return execute_command(line, false);
                    }
                }
            }
            return true;
        }

        // Display response
        if (cmd == "GET") {
            if (resp.payload.find("ERR") != std::string::npos) {
                if (is_interactive_) {
                    std::cout << "\033[1;31m(nil)\033[0m\n";
                } else {
                    std::cout << "(nil)\n";
                }
            } else {
                if (is_interactive_) {
                    std::cout << "\033[1;32m\"" << resp.payload << "\"\033[0m\n";
                } else {
                    std::cout << resp.payload << "\n";
                }
            }
        } else if (cmd == "CLUSTER") {
            if (is_interactive_) {
                std::cout << "\033[1;34m" << resp.payload << "\033[0m\n";
            } else {
                std::cout << resp.payload << "\n";
            }
        } else {
            std::cout << resp.payload << "\n";
        }

        return true;
    }

    void run_repl() {
        std::cout << "\033[1;36m========================================================\033[0m\n";
        std::cout << "\033[1;36m DistriKV Interactive Shell (Berkeley Sockets Client)\033[0m\n";
        std::cout << " Type \033[1;33mHELP\033[0m for commands or \033[1;33mQUIT\033[0m to exit.\n";
        std::cout << "\033[1;36m========================================================\033[0m\n";

        connect();

        std::string line;
        while (true) {
            std::cout << "\033[1;35mdistrikv [" << current_host_ << ":" << current_port_ << "]>\033[0m ";
            if (!std::getline(std::cin, line)) break;
            if (!execute_command(line)) break;
        }
    }

    void run_benchmark(int num_ops) {
        std::cout << "\n\033[1;34m[Benchmark] Running " << num_ops << " PUT & GET operations on " 
                  << current_host_ << ":" << current_port_ << "...\033[0m\n";

        if (!conn_.is_valid() && !connect()) return;

        auto start = std::chrono::high_resolution_clock::now();
        int success_puts = 0;
        int success_gets = 0;

        for (int i = 0; i < num_ops; ++i) {
            std::string key = "bench_k_" + std::to_string(i);
            std::string val = "bench_v_" + std::to_string(i);

            Message put_msg = Protocol::make_put_req(key, val);
            if (Protocol::send_message(conn_.fd(), put_msg)) {
                Message resp;
                if (Protocol::recv_message(conn_.fd(), resp, 2000) && resp.payload == "OK") {
                    success_puts++;
                }
            }
        }

        for (int i = 0; i < num_ops; ++i) {
            std::string key = "bench_k_" + std::to_string(i);
            Message get_msg = Protocol::make_get_req(key);
            if (Protocol::send_message(conn_.fd(), get_msg)) {
                Message resp;
                if (Protocol::recv_message(conn_.fd(), resp, 2000) && resp.payload.find("ERR") == std::string::npos) {
                    success_gets++;
                }
            }
        }

        auto finish = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> duration = finish - start;
        double total_sec = duration.count();
        int total_ops = success_puts + success_gets;
        double ops_sec = (total_sec > 0) ? (total_ops / total_sec) : 0;

        std::cout << "\033[1;32m----------------- Benchmark Results -----------------\033[0m\n";
        std::cout << " Total Time:       " << std::fixed << std::setprecision(3) << total_sec << " s\n";
        std::cout << " Successful PUTs:  " << success_puts << " / " << num_ops << "\n";
        std::cout << " Successful GETs:  " << success_gets << " / " << num_ops << "\n";
        std::cout << " Throughput:       \033[1;33m" << std::fixed << std::setprecision(1) << ops_sec << " ops/sec\033[0m\n";
        std::cout << " Avg Latency:      " << std::fixed << std::setprecision(3) << (total_sec * 1000.0 / total_ops) << " ms/op\n";
        std::cout << "\033[1;32m-----------------------------------------------------\033[0m\n\n";
    }

    void print_help() {
        std::cout << "\n\033[1;33mAvailable Commands:\033[0m\n";
        std::cout << "  PUT <key> <value> [ttl_sec]  - Insert or update a key with optional TTL\n";
        std::cout << "  GET <key>                    - Retrieve the value for a key\n";
        std::cout << "  DEL <key>                    - Delete a key from the cluster\n";
        std::cout << "  KEYS [pattern]               - List all stored keys matching pattern\n";
        std::cout << "  STATS                        - Display storage and memory statistics\n";
        std::cout << "  CLUSTER                      - Inspect cluster topology and election state\n";
        std::cout << "  PING                         - Test round-trip latency to connected node\n";
        std::cout << "  CONNECT <host> <port>        - Connect to another cluster node\n";
        std::cout << "  BENCH [num_ops]              - Run automated throughput stress benchmark\n";
        std::cout << "  QUIT / EXIT                  - Disconnect and exit client\n\n";
    }

private:
    std::string current_host_;
    int current_port_;
    TcpConnection conn_;
};

} // namespace distrikv

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 8001;
    std::string one_shot_cmd;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [--host <ip>] [--port <port>] [command]\n";
            return 0;
        } else {
            if (!one_shot_cmd.empty()) one_shot_cmd += " ";
            one_shot_cmd += arg;
        }
    }

    bool is_interactive = one_shot_cmd.empty();
    distrikv::DistriKvClient client(host, port, is_interactive);

    if (!one_shot_cmd.empty()) {
        client.connect();
        client.execute_command(one_shot_cmd);
        return 0;
    }

    client.run_repl();
    return 0;
}
