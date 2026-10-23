#ifndef DISTRIKV_PROTOCOL_H
#define DISTRIKV_PROTOCOL_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace distrikv {

constexpr uint32_t PROTOCOL_MAGIC = 0x444B5631; // "DKV1" in hex
constexpr uint8_t PROTOCOL_VERSION = 1;

enum class MsgType : uint8_t {
    // Basic Liveness
    PING = 0x01,
    PONG = 0x02,

    // Client KV Commands
    PUT_REQ = 0x10,
    PUT_RESP = 0x11,
    GET_REQ = 0x12,
    GET_RESP = 0x13,
    DEL_REQ = 0x14,
    DEL_RESP = 0x15,
    KEYS_REQ = 0x16,
    KEYS_RESP = 0x17,
    STATS_REQ = 0x18,
    STATS_RESP = 0x19,
    CLUSTER_INFO_REQ = 0x1A,
    CLUSTER_INFO_RESP = 0x1B,

    // Cluster Replication
    REPL_APPEND_REQ = 0x20,
    REPL_APPEND_RESP = 0x21,
    REPL_SYNC_REQ = 0x22,
    REPL_SYNC_RESP = 0x23,

    // Leader Election & Failure Detection
    HEARTBEAT_PING = 0x30,
    HEARTBEAT_PONG = 0x31,
    VOTE_REQ = 0x32,
    VOTE_RESP = 0x33,
    LEADER_ANNOUNCE = 0x34,
    JOIN_REQ = 0x35,
    JOIN_RESP = 0x36,

    // Error / Redirect
    ERROR_RESP = 0x7E,
    REDIRECT_RESP = 0x7F
};

#pragma pack(push, 1)
struct ProtocolHeader {
    uint32_t magic;         // 0x444B5631 ("DKV1")
    uint8_t  version;       // 1
    uint8_t  msg_type;      // MsgType
    uint16_t flags;         // Reserved flags
    uint64_t term;          // Term / Epoch or sequence id
    uint32_t length;        // Payload length in bytes
    uint32_t checksum;      // Header checksum (CRC32 over previous fields)
};
#pragma pack(pop)

static_assert(sizeof(ProtocolHeader) == 24, "ProtocolHeader must be exactly 24 bytes");

/**
 * Message struct representing parsed network frame.
 */
struct Message {
    MsgType msg_type;
    uint16_t flags = 0;
    uint64_t term = 0;
    std::string payload;

    Message() : msg_type(MsgType::PING), flags(0), term(0) {}
    Message(MsgType type, const std::string& p, uint64_t t = 0, uint16_t f = 0)
        : msg_type(type), flags(f), term(t), payload(p) {}
};

class Protocol {
public:
    // Fast CRC32 calculation
    static uint32_t compute_crc32(const uint8_t* data, size_t length);

    // Frame a message into binary buffer [Header + Payload]
    static std::vector<uint8_t> serialize(const Message& msg);

    // Write a message directly to a socket fd
    static bool send_message(int fd, const Message& msg);

    // Read a message from socket fd (handles binary frame or text fallback)
    static bool recv_message(int fd, Message& msg, int timeout_ms = 3000);

    // Convert MsgType to readable string
    static std::string msg_type_to_string(MsgType type);

    // Helper builders for common messages
    static Message make_put_req(const std::string& key, const std::string& val, int64_t ttl_sec = 0);
    static Message make_get_req(const std::string& key);
    static Message make_del_req(const std::string& key);
    static Message make_keys_req(const std::string& pattern = "*");
    static Message make_stats_req();
    static Message make_cluster_info_req();

    static Message make_success_resp(const std::string& data = "OK");
    static Message make_error_resp(const std::string& err_msg);
    static Message make_redirect_resp(const std::string& leader_endpoint);

    // Simple text command parser: splits "PUT foo bar 10" into tokens
    static std::vector<std::string> tokenize(const std::string& line);
};

} // namespace distrikv

#endif // DISTRIKV_PROTOCOL_H
