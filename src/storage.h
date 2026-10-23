#ifndef DISTRIKV_STORAGE_H
#define DISTRIKV_STORAGE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <optional>

namespace distrikv {

struct StorageEntry {
    std::string value;
    uint64_t version = 1;
    int64_t created_at_ms = 0;
    int64_t expires_at_ms = 0; // 0 means no expiration

    bool is_expired(int64_t now_ms) const {
        return (expires_at_ms > 0 && now_ms >= expires_at_ms);
    }
};

enum class LogOpType : uint8_t {
    PUT = 1,
    DEL = 2,
    NOOP = 3
};

struct LogEntry {
    uint64_t index = 0;
    uint64_t term = 0;
    LogOpType op = LogOpType::PUT;
    std::string key;
    std::string value;
    int64_t ttl_sec = 0;
};

struct StorageStats {
    uint64_t total_keys = 0;
    uint64_t log_size = 0;
    size_t memory_bytes = 0;
};

class StorageEngine {
public:
    StorageEngine();
    ~StorageEngine();

    // Core KV Operations (Thread-safe)
    bool put(const std::string& key, const std::string& value, int64_t ttl_sec = 0);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    std::vector<std::string> get_keys(const std::string& pattern = "*", size_t limit = 1000);
    std::vector<std::pair<std::string, std::string>> get_all_entries(const std::string& pattern = "*", size_t limit = 50000);
    bool exists(const std::string& key);

    // Replication Log operations
    uint64_t append_log_entry(uint64_t term, LogOpType op, const std::string& key, const std::string& value, int64_t ttl_sec = 0);
    bool apply_log_entry(const LogEntry& entry);
    std::vector<LogEntry> get_log_entries_from(uint64_t from_index) const;
    uint64_t get_last_log_index() const;
    uint64_t get_last_log_term() const;

    // Snapshot Persistence
    bool save_snapshot(const std::string& filepath);
    bool load_snapshot(const std::string& filepath);

    // Active TTL cleanup
    size_t purge_expired_keys();

    // Telemetry & Stats
    StorageStats get_stats();
    void reset();

private:
    int64_t current_time_ms() const;

    mutable std::shared_mutex rw_lock_;
    std::unordered_map<std::string, StorageEntry> store_;

    mutable std::mutex log_lock_;
    std::vector<LogEntry> log_;
    uint64_t last_log_index_ = 0;
    uint64_t last_log_term_ = 0;
};

} // namespace distrikv

#endif // DISTRIKV_STORAGE_H
