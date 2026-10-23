#include "cluster_node.h"
#include <iostream>
#include <csignal>
#include <cstring>
#include <vector>

static std::unique_ptr<distrikv::ClusterNode> g_node;

void signal_handler(int sig) {
    std::cout << "\n[Main] Caught signal " << sig << ", stopping node...\n";
    if (g_node) {
        g_node->stop();
    }
}

void print_usage(const char* prog) {
    std::cout << "DistriKV - Distributed Fault-Tolerant Key-Value Cluster Node\n";
    std::cout << "Usage: " << prog << " [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --id <int>                Node ID (default: 1)\n";
    std::cout << "  --host <ip>               Listen host IP (default: 127.0.0.1)\n";
    std::cout << "  --port <port>             TCP port for clients/replication (default: 8001)\n";
    std::cout << "  --hb-port <port>          UDP port for heartbeats (default: 9001)\n";
    std::cout << "  --role <PRIMARY|SECONDARY> Initial role (default: SECONDARY)\n";
    std::cout << "  --peer <id,host,tcp,hb>   Register peer node (can repeat)\n";
    std::cout << "  --data-dir <path>         Data directory for persistence (default: data)\n";
    std::cout << "  --help                    Show this help message\n";
    std::cout << "\nExample:\n";
    std::cout << "  " << prog << " --id 1 --port 8001 --hb-port 9001 --role PRIMARY \\\n";
    std::cout << "       --peer 2,127.0.0.1,8002,9002 --peer 3,127.0.0.1,8003,9003\n";
}

int main(int argc, char* argv[]) {
    distrikv::NodeConfig config;
    config.node_id = 1;
    config.host = "127.0.0.1";
    config.tcp_port = 8001;
    config.heartbeat_port = 9001;
    config.initial_role = distrikv::NodeRole::SECONDARY;
    config.data_dir = "data";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--id" && i + 1 < argc) {
            config.node_id = std::stoi(argv[++i]);
        } else if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.tcp_port = std::stoi(argv[++i]);
        } else if (arg == "--hb-port" && i + 1 < argc) {
            config.heartbeat_port = std::stoi(argv[++i]);
        } else if (arg == "--role" && i + 1 < argc) {
            std::string r = argv[++i];
            if (r == "PRIMARY" || r == "primary" || r == "LEADER" || r == "leader") {
                config.initial_role = distrikv::NodeRole::PRIMARY;
            } else {
                config.initial_role = distrikv::NodeRole::SECONDARY;
            }
        } else if (arg == "--peer" && i + 1 < argc) {
            std::string p_str = argv[++i];
            // Format: id,host,tcp,hb
            std::vector<std::string> parts;
            std::string cur;
            for (char c : p_str) {
                if (c == ',') {
                    parts.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            parts.push_back(cur);
            if (parts.size() >= 4) {
                distrikv::PeerConfig p;
                p.id = std::stoi(parts[0]);
                p.host = parts[1];
                p.tcp_port = std::stoi(parts[2]);
                p.heartbeat_port = std::stoi(parts[3]);
                config.peers.push_back(p);
            }
        } else if (arg == "--data-dir" && i + 1 < argc) {
            config.data_dir = argv[++i];
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "========================================================\n";
    std::cout << " DistriKV Node " << config.node_id << " Starting...\n";
    std::cout << " Endpoint: TCP " << config.host << ":" << config.tcp_port 
              << " | UDP HB " << config.heartbeat_port << "\n";
    std::cout << " Initial Role: " << distrikv::role_to_string(config.initial_role) << "\n";
    std::cout << " Registered Peers: " << config.peers.size() << "\n";
    std::cout << "========================================================\n";

    g_node = std::make_unique<distrikv::ClusterNode>(config);
    if (!g_node->start()) {
        std::cerr << "[Main] Failed to start cluster node!\n";
        return 1;
    }

    g_node->wait();
    return 0;
}
