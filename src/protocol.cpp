#include "protocol.h"
#include "socket_util.h"
#include <sstream>
#include <iostream>
#include <cstring>

namespace distrikv {

// Precomputed CRC32 Table
static uint32_t crc32_table[256];
static bool crc32_initialized = false;

static void init_crc32_table() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1) {
                c = 0xEDB88320L ^ (c >> 1);
            } else {
                c = c >> 1;
            }
        }
        crc32_table[i] = c;
    }
    crc32_initialized = true;
}

uint32_t Protocol::compute_crc32(const uint8_t* data, size_t length) {
    if (!crc32_initialized) {
        init_crc32_table();
    }
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

std::vector<uint8_t> Protocol::serialize(const Message& msg) {
    ProtocolHeader hdr;
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.msg_type = static_cast<uint8_t>(msg.msg_type);
    hdr.flags = msg.flags;
    hdr.term = msg.term;
    hdr.length = static_cast<uint32_t>(msg.payload.size());
    
    // Checksum calculated over the first 20 bytes of header (excluding checksum field itself)
    hdr.checksum = compute_crc32(reinterpret_cast<const uint8_t*>(&hdr), 20);

    std::vector<uint8_t> packet;
    packet.resize(sizeof(ProtocolHeader) + msg.payload.size());

    std::memcpy(packet.data(), &hdr, sizeof(ProtocolHeader));
    if (!msg.payload.empty()) {
        std::memcpy(packet.data() + sizeof(ProtocolHeader), msg.payload.data(), msg.payload.size());
    }

    return packet;
}

bool Protocol::send_message(int fd, const Message& msg) {
    std::vector<uint8_t> packet = serialize(msg);
    return SocketUtil::send_all(fd, packet.data(), packet.size());
}

bool Protocol::recv_message(int fd, Message& msg, int timeout_ms) {
    if (timeout_ms > 0) {
        int r = SocketUtil::wait_readable(fd, timeout_ms);
        if (r <= 0) return false;
    }

    // Peek the first 4 bytes to detect binary frame vs text mode
    uint32_t peek_magic = 0;
    ssize_t peek_n = ::recv(fd, &peek_magic, sizeof(peek_magic), MSG_PEEK);
    if (peek_n <= 0) {
        return false;
    }

    if (peek_n >= 4 && peek_magic == PROTOCOL_MAGIC) {
        // Standard Binary Framing
        ProtocolHeader hdr;
        if (!SocketUtil::recv_all(fd, &hdr, sizeof(ProtocolHeader))) {
            return false;
        }

        // Verify Checksum
        uint32_t calculated_checksum = compute_crc32(reinterpret_cast<const uint8_t*>(&hdr), 20);
        if (hdr.checksum != calculated_checksum) {
            std::cerr << "[Protocol] Checksum mismatch! Expected " << hdr.checksum 
                      << ", got " << calculated_checksum << "\n";
            return false;
        }

        msg.msg_type = static_cast<MsgType>(hdr.msg_type);
        msg.flags = hdr.flags;
        msg.term = hdr.term;
        msg.payload.clear();

        if (hdr.length > 0) {
            // Safety guard: max 64MB payload
            if (hdr.length > 64 * 1024 * 1024) {
                std::cerr << "[Protocol] Payload too large: " << hdr.length << " bytes\n";
                return false;
            }
            msg.payload.resize(hdr.length);
            if (!SocketUtil::recv_all(fd, &msg.payload[0], hdr.length)) {
                return false;
            }
        }
        return true;
    } else {
        // Plain text fallback (read line until '\n')
        std::string line;
        char ch;
        while (true) {
            ssize_t n = ::recv(fd, &ch, 1, 0);
            if (n <= 0) {
                if (line.empty()) return false;
                break;
            }
            if (ch == '\r') continue;
            if (ch == '\n') break;
            line.push_back(ch);
            if (line.size() > 65536) break; // line length limit
        }

        auto tokens = tokenize(line);
        if (tokens.empty()) {
            msg.msg_type = MsgType::PING;
            msg.payload = "";
            return true;
        }

        std::string cmd = tokens[0];
        for (auto& c : cmd) c = toupper(c);

        if (cmd == "PUT" && tokens.size() >= 3) {
            msg.msg_type = MsgType::PUT_REQ;
            int64_t ttl = 0;
            if (tokens.size() >= 4) {
                try { ttl = std::stoll(tokens[3]); } catch (...) {}
            }
            msg.payload = tokens[1] + "\t" + tokens[2] + "\t" + std::to_string(ttl);
        } else if (cmd == "GET" && tokens.size() >= 2) {
            msg.msg_type = MsgType::GET_REQ;
            msg.payload = tokens[1];
        } else if (cmd == "DEL" && tokens.size() >= 2) {
            msg.msg_type = MsgType::DEL_REQ;
            msg.payload = tokens[1];
        } else if (cmd == "KEYS") {
            msg.msg_type = MsgType::KEYS_REQ;
            msg.payload = tokens.size() >= 2 ? tokens[1] : "*";
        } else if (cmd == "STATS") {
            msg.msg_type = MsgType::STATS_REQ;
            msg.payload = "";
        } else if (cmd == "CLUSTER") {
            msg.msg_type = MsgType::CLUSTER_INFO_REQ;
            msg.payload = "";
        } else if (cmd == "PING") {
            msg.msg_type = MsgType::PING;
            msg.payload = "PONG";
        } else {
            msg.msg_type = MsgType::PING;
            msg.payload = line;
        }
        return true;
    }
}

std::string Protocol::msg_type_to_string(MsgType type) {
    switch (type) {
        case MsgType::PING: return "PING";
        case MsgType::PONG: return "PONG";
        case MsgType::PUT_REQ: return "PUT_REQ";
        case MsgType::PUT_RESP: return "PUT_RESP";
        case MsgType::GET_REQ: return "GET_REQ";
        case MsgType::GET_RESP: return "GET_RESP";
        case MsgType::DEL_REQ: return "DEL_REQ";
        case MsgType::DEL_RESP: return "DEL_RESP";
        case MsgType::KEYS_REQ: return "KEYS_REQ";
        case MsgType::KEYS_RESP: return "KEYS_RESP";
        case MsgType::STATS_REQ: return "STATS_REQ";
        case MsgType::STATS_RESP: return "STATS_RESP";
        case MsgType::CLUSTER_INFO_REQ: return "CLUSTER_INFO_REQ";
        case MsgType::CLUSTER_INFO_RESP: return "CLUSTER_INFO_RESP";
        case MsgType::REPL_APPEND_REQ: return "REPL_APPEND_REQ";
        case MsgType::REPL_APPEND_RESP: return "REPL_APPEND_RESP";
        case MsgType::REPL_SYNC_REQ: return "REPL_SYNC_REQ";
        case MsgType::REPL_SYNC_RESP: return "REPL_SYNC_RESP";
        case MsgType::HEARTBEAT_PING: return "HEARTBEAT_PING";
        case MsgType::HEARTBEAT_PONG: return "HEARTBEAT_PONG";
        case MsgType::VOTE_REQ: return "VOTE_REQ";
        case MsgType::VOTE_RESP: return "VOTE_RESP";
        case MsgType::LEADER_ANNOUNCE: return "LEADER_ANNOUNCE";
        case MsgType::JOIN_REQ: return "JOIN_REQ";
        case MsgType::JOIN_RESP: return "JOIN_RESP";
        case MsgType::ERROR_RESP: return "ERROR_RESP";
        case MsgType::REDIRECT_RESP: return "REDIRECT_RESP";
        default: return "UNKNOWN";
    }
}

Message Protocol::make_put_req(const std::string& key, const std::string& val, int64_t ttl_sec) {
    return Message(MsgType::PUT_REQ, key + "\t" + val + "\t" + std::to_string(ttl_sec));
}

Message Protocol::make_get_req(const std::string& key) {
    return Message(MsgType::GET_REQ, key);
}

Message Protocol::make_del_req(const std::string& key) {
    return Message(MsgType::DEL_REQ, key);
}

Message Protocol::make_keys_req(const std::string& pattern) {
    return Message(MsgType::KEYS_REQ, pattern);
}

Message Protocol::make_stats_req() {
    return Message(MsgType::STATS_REQ, "");
}

Message Protocol::make_cluster_info_req() {
    return Message(MsgType::CLUSTER_INFO_REQ, "");
}

Message Protocol::make_success_resp(const std::string& data) {
    return Message(MsgType::PUT_RESP, data);
}

Message Protocol::make_error_resp(const std::string& err_msg) {
    return Message(MsgType::ERROR_RESP, err_msg);
}

Message Protocol::make_redirect_resp(const std::string& leader_endpoint) {
    return Message(MsgType::REDIRECT_RESP, leader_endpoint);
}

std::vector<std::string> Protocol::tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

} // namespace distrikv
