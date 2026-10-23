#include "cluster_node.h"
#include <iostream>
#include <sstream>
#include <random>
#include <chrono>
#include <thread>
#include <cstring>
#include <algorithm>

namespace distrikv {

ClusterNode::ClusterNode(const NodeConfig& config)
    : config_(config),
      role_(config.initial_role),
      current_term_(0),
      current_leader_id_(-1) {

    for (const auto& peer : config_.peers) {
        auto ps = std::make_shared<PeerState>();
        ps->config = peer;
        ps->is_alive = false;
        ps->last_seen_ms = 0;
        ps->match_index = 0;
        peers_[peer.id] = ps;
    }

    if (role_ == NodeRole::PRIMARY) {
        current_leader_id_ = config_.node_id;
    }

    current_election_timeout_ms_ = get_random_election_timeout_ms();
    last_heartbeat_received_ms_ = now_ms();
    start_time_ = std::chrono::system_clock::now();
}

ClusterNode::~ClusterNode() {
    stop();
}

int64_t ClusterNode::now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int ClusterNode::get_random_election_timeout_ms() const {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(config_.election_timeout_min_ms, config_.election_timeout_max_ms);
    return dist(gen);
}

std::string ClusterNode::get_snapshot_path() const {
    return config_.data_dir + "/dump_node_" + std::to_string(config_.node_id) + ".kv";
}

NodeRole ClusterNode::get_role() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return role_;
}

std::string ClusterNode::get_leader_endpoint() const {
    int leader_id = current_leader_id_.load();
    if (leader_id == config_.node_id) {
        return config_.host + ":" + std::to_string(config_.tcp_port);
    }
    auto it = peers_.find(leader_id);
    if (it != peers_.end()) {
        return it->second->config.host + ":" + std::to_string(it->second->config.tcp_port);
    }
    return "UNKNOWN";
}

bool ClusterNode::start() {
    if (running_.load()) return true;
    running_ = true;

    // 1. Try to load existing snapshot from disk
    std::string snap_file = get_snapshot_path();
    if (storage_.load_snapshot(snap_file)) {
        std::cout << "[Node " << config_.node_id << "] Loaded snapshot from " << snap_file << "\n";
    }

    // 2. Start TCP Listener
    if (!tcp_listener_.listen_on(config_.host, config_.tcp_port)) {
        std::cerr << "[Node " << config_.node_id << "] Failed to bind TCP listener on " 
                  << config_.host << ":" << config_.tcp_port << "\n";
        running_ = false;
        return false;
    }
    std::cout << "[Node " << config_.node_id << "] TCP Server listening on " 
              << config_.host << ":" << config_.tcp_port << " [Role: " << role_to_string(role_) << "]\n";

    // 3. Start UDP Heartbeat Socket
    if (!udp_heartbeat_socket_.bind_to(config_.host, config_.heartbeat_port)) {
        std::cerr << "[Node " << config_.node_id << "] Warning: Failed to bind UDP on " 
                  << config_.host << ":" << config_.heartbeat_port << "\n";
    } else {
        std::cout << "[Node " << config_.node_id << "] UDP Heartbeat listener bound on " 
                  << config_.host << ":" << config_.heartbeat_port << "\n";
    }

    // 4. Spawn Background Threads
    tcp_thread_ = std::thread(&ClusterNode::run_tcp_server, this);
    udp_thread_ = std::thread(&ClusterNode::run_udp_heartbeat_listener, this);
    timer_thread_ = std::thread(&ClusterNode::run_election_heartbeat_timer, this);
    maint_thread_ = std::thread(&ClusterNode::run_background_maintenance, this);

    // If starting as follower, trigger background sync from leader
    if (role_ == NodeRole::SECONDARY) {
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            sync_follower_from_leader();
        }).detach();
    }

    return true;
}

void ClusterNode::stop() {
    if (!running_.exchange(false)) return;

    std::cout << "[Node " << config_.node_id << "] Shutting down gracefully...\n";

    // Close listener sockets to break blocking select/accept calls
    tcp_listener_.close();
    udp_heartbeat_socket_.close();

    // Persist snapshot to disk
    std::string snap_file = get_snapshot_path();
    if (storage_.save_snapshot(snap_file)) {
        std::cout << "[Node " << config_.node_id << "] Saved snapshot to " << snap_file << "\n";
    }

    if (tcp_thread_.joinable()) tcp_thread_.join();
    if (udp_thread_.joinable()) udp_thread_.join();
    if (timer_thread_.joinable()) timer_thread_.join();
    if (maint_thread_.joinable()) maint_thread_.join();

    std::cout << "[Node " << config_.node_id << "] Stopped successfully.\n";
}

void ClusterNode::wait() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void ClusterNode::run_tcp_server() {
    while (running_.load()) {
        int ready = SocketUtil::wait_readable(tcp_listener_.fd(), 500);
        if (!running_.load()) break;
        if (ready <= 0) continue;

        std::string client_ip;
        int client_port = 0;
        auto client_sock = tcp_listener_.accept_connection(&client_ip, &client_port);
        if (client_sock && client_sock->is_valid()) {
            std::thread(&ClusterNode::handle_client_connection, this,
                        std::move(client_sock), client_ip, client_port).detach();
        }
    }
}

void ClusterNode::handle_client_connection(std::unique_ptr<Socket> client_sock, std::string client_ip, int client_port) {
    (void)client_ip;
    (void)client_port;
    int fd = client_sock->fd();

    while (running_.load()) {
        Message req;
        if (!Protocol::recv_message(fd, req, 30000)) {
            break; // Connection closed or timed out
        }

        Message resp = process_message(req);
        if (!Protocol::send_message(fd, resp)) {
            break;
        }
    }
}

Message ClusterNode::process_message(const Message& req, bool is_peer) {
    (void)is_peer;
    switch (req.msg_type) {
        case MsgType::PING:
            return Message(MsgType::PONG, "PONG");
        case MsgType::PUT_REQ:
            return handle_kv_put(req);
        case MsgType::GET_REQ:
            return handle_kv_get(req);
        case MsgType::DEL_REQ:
            return handle_kv_del(req);
        case MsgType::KEYS_REQ:
            return handle_kv_keys(req);
        case MsgType::STATS_REQ:
            return handle_kv_stats(req);
        case MsgType::CLUSTER_INFO_REQ:
            return handle_cluster_info(req);
        case MsgType::REPL_APPEND_REQ:
            return handle_repl_append(req);
        case MsgType::REPL_SYNC_REQ: {
            auto entries = storage_.get_all_entries("*", 50000);
            std::ostringstream oss;
            for (const auto& [k, v] : entries) {
                oss << k << "\t" << v << "\n";
            }
            return Message(MsgType::REPL_SYNC_RESP, oss.str(), current_term_.load());
        }
        case MsgType::VOTE_REQ:
            return handle_vote_request(req);
        case MsgType::LEADER_ANNOUNCE:
            return handle_leader_announce(req);
        default:
            return Protocol::make_error_resp("UNKNOWN_COMMAND");
    }
}

Message ClusterNode::handle_kv_put(const Message& req) {
    NodeRole cur_role;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        cur_role = role_;
    }

    if (cur_role != NodeRole::PRIMARY) {
        // Redirect to Primary
        return Protocol::make_redirect_resp("MOVED " + get_leader_endpoint());
    }

    // Parse payload: "key\tvalue\tttl"
    std::string key, value;
    int64_t ttl_sec = 0;
    auto pos1 = req.payload.find('\t');
    if (pos1 == std::string::npos) {
        return Protocol::make_error_resp("ERR syntax: PUT <key> <val> [ttl]");
    }
    key = req.payload.substr(0, pos1);
    auto pos2 = req.payload.find('\t', pos1 + 1);
    if (pos2 == std::string::npos) {
        value = req.payload.substr(pos1 + 1);
    } else {
        value = req.payload.substr(pos1 + 1, pos2 - (pos1 + 1));
        try {
            ttl_sec = std::stoll(req.payload.substr(pos2 + 1));
        } catch (...) {}
    }

    uint64_t term = current_term_.load();
    // 1. Apply to local storage
    storage_.put(key, value, ttl_sec);
    storage_.append_log_entry(term, LogOpType::PUT, key, value, ttl_sec);

    // 2. Broadcast replication to followers over Berkeley Sockets
    replicate_to_peers(term, LogOpType::PUT, key, value, ttl_sec);

    // Track successful client PUT operation
    client_put_count_++;
    client_ops_count_++;

    return Protocol::make_success_resp("OK");
}

Message ClusterNode::handle_kv_get(const Message& req) {
    std::string key = req.payload;
    auto val = storage_.get(key);
    if (val.has_value()) {
        // Track successful client GET operation
        client_get_count_++;
        client_ops_count_++;
        return Message(MsgType::GET_RESP, *val);
    } else {
        return Message(MsgType::GET_RESP, "ERR Key not found");
    }
}

Message ClusterNode::handle_kv_del(const Message& req) {
    NodeRole cur_role;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        cur_role = role_;
    }

    if (cur_role != NodeRole::PRIMARY) {
        return Protocol::make_redirect_resp("MOVED " + get_leader_endpoint());
    }

    std::string key = req.payload;
    uint64_t term = current_term_.load();
    bool removed = storage_.del(key);
    
    if (removed) {
        storage_.append_log_entry(term, LogOpType::DEL, key, "", 0);
        replicate_to_peers(term, LogOpType::DEL, key, "", 0);
        // Track successful client DEL operation
        client_del_count_++;
        client_ops_count_++;
        return Message(MsgType::DEL_RESP, "OK");
    } else {
        return Message(MsgType::DEL_RESP, "ERR Key not found");
    }
}

Message ClusterNode::handle_kv_keys(const Message& req) {
    std::string pattern = req.payload.empty() ? "*" : req.payload;
    
    // Support "DUMP" or "DATA" for dashboard/monitoring single-trip fetch
    if (pattern == "DUMP" || pattern == "DATA") {
        auto entries = storage_.get_all_entries("*", 50000);
        std::ostringstream oss;
        for (size_t i = 0; i < entries.size(); ++i) {
            oss << entries[i].first << "\t" << entries[i].second;
            if (i + 1 < entries.size()) oss << "\n";
        }
        return Message(MsgType::KEYS_RESP, oss.str());
    }

    auto keys = storage_.get_keys(pattern);
    std::ostringstream oss;
    for (size_t i = 0; i < keys.size(); ++i) {
        oss << keys[i];
        if (i + 1 < keys.size()) oss << "\n";
    }
    return Message(MsgType::KEYS_RESP, oss.str());
}

Message ClusterNode::handle_kv_stats(const Message& req) {
    (void)req;
    auto stats = storage_.get_stats();
    std::ostringstream oss;
    oss << "Node ID: " << config_.node_id << "\n"
        << "Role: " << role_to_string(get_role()) << "\n"
        << "Term: " << current_term_.load() << "\n"
        << "Leader: " << current_leader_id_.load() << " (" << get_leader_endpoint() << ")\n"
        << "Total Keys: " << stats.total_keys << "\n"
        << "Total Client Ops Handled: " << client_ops_count_.load() << "\n"
        << "Client Puts: " << client_put_count_.load() 
        << " | Client Gets: " << client_get_count_.load() 
        << " | Client Dels: " << client_del_count_.load() << "\n"
        << "Replication Log Index: " << storage_.get_last_log_index() << "\n"
        << "Memory Approx: " << stats.memory_bytes << " bytes\n";
    return Message(MsgType::STATS_RESP, oss.str());
}

Message ClusterNode::handle_cluster_info(const Message& req) {
    (void)req;
    return Message(MsgType::CLUSTER_INFO_RESP, get_status_json());
}

std::string ClusterNode::get_status_json() {
    auto stats = storage_.get_stats();
    auto now = std::chrono::system_clock::now();
    auto uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();

    std::ostringstream oss;
    oss << "{\n"
        << "  \"node_id\": " << config_.node_id << ",\n"
        << "  \"role\": \"" << role_to_string(get_role()) << "\",\n"
        << "  \"term\": " << current_term_.load() << ",\n"
        << "  \"leader_id\": " << current_leader_id_.load() << ",\n"
        << "  \"leader_endpoint\": \"" << get_leader_endpoint() << "\",\n"
        << "  \"host\": \"" << config_.host << "\",\n"
        << "  \"tcp_port\": " << config_.tcp_port << ",\n"
        << "  \"heartbeat_port\": " << config_.heartbeat_port << ",\n"
        << "  \"uptime_seconds\": " << uptime_sec << ",\n"
        << "  \"total_keys\": " << stats.total_keys << ",\n"
        << "  \"client_ops\": " << client_ops_count_.load() << ",\n"
        << "  \"client_put_count\": " << client_put_count_.load() << ",\n"
        << "  \"client_get_count\": " << client_get_count_.load() << ",\n"
        << "  \"client_del_count\": " << client_del_count_.load() << ",\n"
        << "  \"put_count\": " << client_put_count_.load() << ",\n"
        << "  \"get_count\": " << client_get_count_.load() << ",\n"
        << "  \"del_count\": " << client_del_count_.load() << ",\n"
        << "  \"last_log_index\": " << storage_.get_last_log_index() << ",\n"
        << "  \"memory_bytes\": " << stats.memory_bytes << ",\n"
        << "  \"peers\": [\n";

    size_t i = 0;
    for (const auto& [id, ps] : peers_) {
        bool alive = ps->is_alive.load();
        int64_t last_seen_ago = now_ms() - ps->last_seen_ms.load();
        oss << "    {\n"
            << "      \"id\": " << id << ",\n"
            << "      \"host\": \"" << ps->config.host << "\",\n"
            << "      \"tcp_port\": " << ps->config.tcp_port << ",\n"
            << "      \"heartbeat_port\": " << ps->config.heartbeat_port << ",\n"
            << "      \"alive\": " << (alive ? "true" : "false") << ",\n"
            << "      \"last_seen_ms_ago\": " << (ps->last_seen_ms.load() > 0 ? last_seen_ago : -1) << "\n"
            << "    }" << (i + 1 < peers_.size() ? "," : "") << "\n";
        i++;
    }

    oss << "  ]\n"
        << "}";
    return oss.str();
}

Message ClusterNode::handle_repl_append(const Message& req) {
    // Format: term\tindex\top\tkey\tvalue\tttl
    std::istringstream iss(req.payload);
    std::string term_str, index_str, op_str, key, value, ttl_str;
    if (std::getline(iss, term_str, '\t') &&
        std::getline(iss, index_str, '\t') &&
        std::getline(iss, op_str, '\t') &&
        std::getline(iss, key, '\t')) {

        std::getline(iss, value, '\t');
        std::getline(iss, ttl_str, '\t');

        uint64_t term = std::stoull(term_str);
        uint64_t index = std::stoull(index_str);
        int op_int = std::stoi(op_str);
        int64_t ttl = ttl_str.empty() ? 0 : std::stoll(ttl_str);

        if (term >= current_term_.load()) {
            current_term_ = term;
            last_heartbeat_received_ms_ = now_ms();
            if (role_ != NodeRole::SECONDARY) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                role_ = NodeRole::SECONDARY;
            }
        }

        LogEntry entry;
        entry.term = term;
        entry.index = index;
        entry.op = static_cast<LogOpType>(op_int);
        entry.key = key;
        entry.value = value;
        entry.ttl_sec = ttl;

        // Apply replication entry locally (does not increment client operation counters)
        storage_.apply_log_entry(entry);
        return Message(MsgType::REPL_APPEND_RESP, "ACK");
    }

    return Message(MsgType::ERROR_RESP, "ERR malformed replication payload");
}

bool ClusterNode::replicate_to_peers(uint64_t term, LogOpType op, const std::string& key, const std::string& value, int64_t ttl_sec) {
    uint64_t log_idx = storage_.get_last_log_index();
    std::ostringstream payload_oss;
    payload_oss << term << "\t" << log_idx << "\t" << static_cast<int>(op) << "\t"
                << key << "\t" << value << "\t" << ttl_sec;

    Message msg(MsgType::REPL_APPEND_REQ, payload_oss.str(), term);

    for (auto& [id, ps] : peers_) {
        TcpConnection conn;
        if (conn.connect_to(ps->config.host, ps->config.tcp_port, 800)) {
            Protocol::send_message(conn.fd(), msg);
            Message ack;
            if (Protocol::recv_message(conn.fd(), ack, 800)) {
                if (ack.msg_type == MsgType::REPL_APPEND_RESP) {
                    ps->is_alive = true;
                    ps->last_seen_ms = now_ms();
                    ps->match_index = log_idx;
                }
            }
        } else {
            ps->is_alive = false;
        }
    }
    return true;
}

bool ClusterNode::sync_follower_from_leader() {
    int leader_id = current_leader_id_.load();
    if (leader_id <= 0 || leader_id == config_.node_id) return false;

    auto it = peers_.find(leader_id);
    if (it == peers_.end()) return false;

    TcpConnection conn;
    if (!conn.connect_to(it->second->config.host, it->second->config.tcp_port, 1500)) {
        return false;
    }

    Message req(MsgType::REPL_SYNC_REQ, "", current_term_.load());
    if (!Protocol::send_message(conn.fd(), req)) return false;

    Message resp;
    if (!Protocol::recv_message(conn.fd(), resp, 3000)) return false;

    if (resp.msg_type == MsgType::REPL_SYNC_RESP) {
        std::istringstream iss(resp.payload);
        std::string line;
        size_t count = 0;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            auto tab = line.find('\t');
            if (tab != std::string::npos) {
                std::string k = line.substr(0, tab);
                std::string v = line.substr(tab + 1);
                storage_.put(k, v);
                count++;
            }
        }
        std::cout << "[Node " << config_.node_id << "] Synchronized " << count 
                  << " keys from leader Node " << leader_id << "\n";
        return true;
    }
    return false;
}

void ClusterNode::run_udp_heartbeat_listener() {
    std::vector<char> buffer(4096);
    while (running_.load()) {
        std::string src_host;
        int src_port = 0;
        ssize_t bytes = udp_heartbeat_socket_.recv_from(buffer.data(), buffer.size() - 1, src_host, src_port, 500);
        if (!running_.load()) break;
        if (bytes > 0) {
            buffer[bytes] = '\0';
            std::string packet(buffer.data(), bytes);
            handle_udp_heartbeat(packet, src_host, src_port);
        }
    }
}

void ClusterNode::handle_udp_heartbeat(const std::string& packet, const std::string& src_host, int src_port) {
    std::istringstream iss(packet);
    std::string type, id_str, term_str;
    if (std::getline(iss, type, '|') && std::getline(iss, id_str, '|') && std::getline(iss, term_str, '|')) {
        int peer_id = std::stoi(id_str);
        uint64_t peer_term = std::stoull(term_str);

        auto it = peers_.find(peer_id);
        if (it != peers_.end()) {
            it->second->is_alive = true;
            it->second->last_seen_ms = now_ms();
        }

        if (type == "PING") {
            if (peer_term >= current_term_.load()) {
                current_term_ = peer_term;
                current_leader_id_ = peer_id;
                last_heartbeat_received_ms_ = now_ms();

                std::lock_guard<std::mutex> lock(state_mutex_);
                if (role_ != NodeRole::SECONDARY) {
                    std::cout << "[Node " << config_.node_id << "] Stepping down to SECONDARY under Leader " 
                              << peer_id << " (Term " << peer_term << ")\n";
                    role_ = NodeRole::SECONDARY;
                }
            }

            std::string pong = "PONG|" + std::to_string(config_.node_id) + "|" + std::to_string(current_term_.load());
            udp_heartbeat_socket_.send_to(pong.data(), pong.size(), src_host, src_port);
        } else if (type == "PONG") {
            if (it != peers_.end()) {
                it->second->is_alive = true;
                it->second->last_seen_ms = now_ms();
            }
        }
    }
}

void ClusterNode::send_heartbeats() {
    std::string ping = "PING|" + std::to_string(config_.node_id) + "|" 
                       + std::to_string(current_term_.load()) + "|" 
                       + std::to_string(storage_.get_last_log_index());

    for (auto& [id, ps] : peers_) {
        udp_heartbeat_socket_.send_to(ping.data(), ping.size(), ps->config.host, ps->config.heartbeat_port);
    }
}

void ClusterNode::run_election_heartbeat_timer() {
    int64_t last_heartbeat_sent_ms = 0;

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running_.load()) break;

        int64_t now = now_ms();
        NodeRole cur_role = get_role();

        if (cur_role == NodeRole::PRIMARY) {
            if (now - last_heartbeat_sent_ms >= config_.heartbeat_interval_ms) {
                send_heartbeats();
                last_heartbeat_sent_ms = now;
            }
        } else {
            int64_t elapsed = now - last_heartbeat_received_ms_.load();
            if (elapsed > current_election_timeout_ms_) {
                std::cout << "[Node " << config_.node_id << "] Heartbeat timeout (" 
                          << elapsed << "ms > " << current_election_timeout_ms_ << "ms). Triggering election!\n";
                start_election();
            }
        }
    }
}

void ClusterNode::start_election() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        role_ = NodeRole::CANDIDATE;
        current_term_++;
        voted_for_term_id_ = config_.node_id;
        voted_term_ = current_term_.load();
    }

    current_leader_id_ = -1;
    last_heartbeat_received_ms_ = now_ms();
    current_election_timeout_ms_ = get_random_election_timeout_ms();

    uint64_t term = current_term_.load();
    uint64_t last_idx = storage_.get_last_log_index();
    uint64_t last_term = storage_.get_last_log_term();

    std::cout << "[Node " << config_.node_id << "] Starting Election for Term " << term << "...\n";

    int votes = 1; // Vote for self
    int total_nodes = static_cast<int>(peers_.size()) + 1;
    int needed_votes = (total_nodes / 2) + 1;

    std::ostringstream oss;
    oss << config_.node_id << "\t" << term << "\t" << last_idx << "\t" << last_term;
    Message vote_req(MsgType::VOTE_REQ, oss.str(), term);

    for (auto& [id, ps] : peers_) {
        TcpConnection conn;
        if (conn.connect_to(ps->config.host, ps->config.tcp_port, 800)) {
            Protocol::send_message(conn.fd(), vote_req);
            Message vote_resp;
            if (Protocol::recv_message(conn.fd(), vote_resp, 800)) {
                if (vote_resp.msg_type == MsgType::VOTE_RESP && vote_resp.payload == "GRANTED") {
                    votes++;
                    ps->is_alive = true;
                    ps->last_seen_ms = now_ms();
                }
            }
        }
    }

    std::cout << "[Node " << config_.node_id << "] Received " << votes << " / " << total_nodes 
              << " votes (needed: " << needed_votes << ")\n";

    if (votes >= needed_votes) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            role_ = NodeRole::PRIMARY;
            current_leader_id_ = config_.node_id;
        }

        std::cout << ">>> [Node " << config_.node_id << "] WON ELECTION! Became new PRIMARY for Term " 
                  << term << " <<<\n";

        Message announce(MsgType::LEADER_ANNOUNCE, std::to_string(config_.node_id), term);
        for (auto& [id, ps] : peers_) {
            TcpConnection conn;
            if (conn.connect_to(ps->config.host, ps->config.tcp_port, 800)) {
                Protocol::send_message(conn.fd(), announce);
            }
        }

        send_heartbeats();
    } else {
        std::lock_guard<std::mutex> lock(state_mutex_);
        role_ = NodeRole::SECONDARY;
    }
}

Message ClusterNode::handle_vote_request(const Message& req) {
    std::istringstream iss(req.payload);
    std::string cand_id_str, cand_term_str, cand_idx_str, cand_term_last_str;
    if (std::getline(iss, cand_id_str, '\t') &&
        std::getline(iss, cand_term_str, '\t') &&
        std::getline(iss, cand_idx_str, '\t') &&
        std::getline(iss, cand_term_last_str, '\t')) {

        int cand_id = std::stoi(cand_id_str);
        uint64_t cand_term = std::stoull(cand_term_str);
        uint64_t cand_last_idx = std::stoull(cand_idx_str);

        std::lock_guard<std::mutex> lock(state_mutex_);

        if (cand_term > current_term_.load()) {
            current_term_ = cand_term;
            role_ = NodeRole::SECONDARY;
            voted_for_term_id_ = -1;
            voted_term_ = 0;
        }

        bool can_vote = (voted_term_ < cand_term || voted_for_term_id_ == -1 || voted_for_term_id_ == cand_id);
        bool log_up_to_date = (cand_last_idx >= storage_.get_last_log_index());

        if (cand_term >= current_term_.load() && can_vote && log_up_to_date) {
            voted_for_term_id_ = cand_id;
            voted_term_ = cand_term;
            last_heartbeat_received_ms_ = now_ms();
            std::cout << "[Node " << config_.node_id << "] Granted vote to Candidate " 
                      << cand_id << " for Term " << cand_term << "\n";
            return Message(MsgType::VOTE_RESP, "GRANTED", current_term_.load());
        }
    }

    return Message(MsgType::VOTE_RESP, "DENIED", current_term_.load());
}

Message ClusterNode::handle_leader_announce(const Message& req) {
    int new_leader = std::stoi(req.payload);
    uint64_t leader_term = req.term;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (leader_term >= current_term_.load()) {
            current_term_ = leader_term;
            current_leader_id_ = new_leader;
            role_ = NodeRole::SECONDARY;
            last_heartbeat_received_ms_ = now_ms();
            std::cout << "[Node " << config_.node_id << "] Acknowledged new PRIMARY Leader " 
                      << new_leader << " for Term " << leader_term << "\n";
        }
    }
    return Message(MsgType::PING, "ACK");
}

void ClusterNode::run_background_maintenance() {
    int64_t last_snapshot_time = now_ms();

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!running_.load()) break;

        size_t purged = storage_.purge_expired_keys();
        if (purged > 0) {
            std::cout << "[Node " << config_.node_id << "] Purged " << purged << " expired keys\n";
        }

        int64_t now = now_ms();
        if (now - last_snapshot_time >= 30000) {
            storage_.save_snapshot(get_snapshot_path());
            last_snapshot_time = now;
        }
    }
}

void ClusterNode::step_down(uint64_t new_term) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (new_term > current_term_.load()) {
        current_term_ = new_term;
    }
    role_ = NodeRole::SECONDARY;
    last_heartbeat_received_ms_ = now_ms();
}

void ClusterNode::trigger_election() {
    start_election();
}

} // namespace distrikv
