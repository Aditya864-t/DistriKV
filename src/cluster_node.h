#ifndef DISTRIKV_CLUSTER_NODE_H
#define DISTRIKV_CLUSTER_NODE_H

#include "socket_util.h"
#include "protocol.h"
#include "storage.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

namespace distrikv {

enum class NodeRole {
    PRIMARY,
    SECONDARY,
    CANDIDATE
};

inline std::string role_to_string(NodeRole role) {
    switch (role) {
        case NodeRole::PRIMARY: return "PRIMARY";
        case NodeRole::SECONDARY: return "SECONDARY";
        case NodeRole::CANDIDATE: return "CANDIDATE";
        default: return "UNKNOWN";
    }
}

struct PeerConfig {
    int id = 0;
    std::string host = "127.0.0.1";
    int tcp_port = 8000;
    int heartbeat_port = 9000;
};

struct PeerState {
    PeerConfig config;
    std::atomic<bool> is_alive{false};
    std::atomic<int64_t> last_seen_ms{0};
    std::atomic<uint64_t> match_index{0};
};

struct NodeConfig {
    int node_id = 1;
    std::string host = "127.0.0.1";
    int tcp_port = 8001;
    int heartbeat_port = 9001;
    NodeRole initial_role = NodeRole::SECONDARY;
    std::vector<PeerConfig> peers;
    std::string data_dir = "data";
    int heartbeat_interval_ms = 500;
    int election_timeout_min_ms = 1800;
    int election_timeout_max_ms = 3200;
};

class ClusterNode {
public:
    explicit ClusterNode(const NodeConfig& config);
    ~ClusterNode();

    // Start node servers and background workers
    bool start();

    // Stop node cleanly
    void stop();

    // Wait until node stops
    void wait();

    // Role and info inspection
    NodeRole get_role() const;
    int get_node_id() const { return config_.node_id; }
    uint64_t get_current_term() const { return current_term_.load(); }
    int get_leader_id() const { return current_leader_id_.load(); }
    std::string get_leader_endpoint() const;
    std::string get_status_json();

    // Force step down / leadership change
    void step_down(uint64_t new_term);
    void trigger_election();

private:
    // Main event loops
    void run_tcp_server();
    void run_udp_heartbeat_listener();
    void run_election_heartbeat_timer();
    void run_background_maintenance();

    // Connection handler
    void handle_client_connection(std::unique_ptr<Socket> client_sock, std::string client_ip, int client_port);

    // Message dispatchers
    Message process_message(const Message& req, bool is_peer = false);
    Message handle_kv_put(const Message& req);
    Message handle_kv_get(const Message& req);
    Message handle_kv_del(const Message& req);
    Message handle_kv_keys(const Message& req);
    Message handle_kv_stats(const Message& req);
    Message handle_cluster_info(const Message& req);

    // Replication logic
    Message handle_repl_append(const Message& req);
    bool replicate_to_peers(uint64_t term, LogOpType op, const std::string& key, const std::string& value, int64_t ttl_sec);
    bool sync_follower_from_leader();

    // Election logic
    void handle_udp_heartbeat(const std::string& packet, const std::string& src_host, int src_port);
    void send_heartbeats();
    void start_election();
    Message handle_vote_request(const Message& req);
    Message handle_leader_announce(const Message& req);

    // Utility
    int64_t now_ms() const;
    int get_random_election_timeout_ms() const;
    std::string get_snapshot_path() const;

    NodeConfig config_;
    StorageEngine storage_;
    std::atomic<bool> running_{false};

    // Client operation metrics (only successful client-requested PUT, GET, DEL)
    std::atomic<uint64_t> client_put_count_{0};
    std::atomic<uint64_t> client_get_count_{0};
    std::atomic<uint64_t> client_del_count_{0};
    std::atomic<uint64_t> client_ops_count_{0};

    // Consensus & Role state
    mutable std::mutex state_mutex_;
    NodeRole role_;
    std::atomic<uint64_t> current_term_{0};
    std::atomic<int> current_leader_id_{-1};
    int voted_for_term_id_ = -1;
    uint64_t voted_term_ = 0;
    std::atomic<int64_t> last_heartbeat_received_ms_{0};
    int current_election_timeout_ms_ = 2000;

    // Peers map
    std::unordered_map<int, std::shared_ptr<PeerState>> peers_;

    // Sockets
    TcpListener tcp_listener_;
    UdpSocket udp_heartbeat_socket_;

    // Threads
    std::thread tcp_thread_;
    std::thread udp_thread_;
    std::thread timer_thread_;
    std::thread maint_thread_;

    // Node start time for uptime
    std::chrono::system_clock::time_point start_time_;
};

} // namespace distrikv

#endif // DISTRIKV_CLUSTER_NODE_H
