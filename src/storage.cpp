#include "storage.h"
#include "protocol.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdio>

namespace distrikv {

constexpr const char* SNAPSHOT_MAGIC = "DKVSNAP1";

StorageEngine::StorageEngine() {}

StorageEngine::~StorageEngine() {}

int64_t StorageEngine::current_time_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool StorageEngine::put(const std::string& key, const std::string& value, int64_t ttl_sec) {
    int64_t now = current_time_ms();
    int64_t expires_at = (ttl_sec > 0) ? (now + ttl_sec * 1000) : 0;

    std::unique_lock<std::shared_mutex> lock(rw_lock_);
    auto it = store_.find(key);
    if (it != store_.end()) {
        it->second.value = value;
        it->second.version++;
        it->second.created_at_ms = now;
        it->second.expires_at_ms = expires_at;
    } else {
        StorageEntry entry;
        entry.value = value;
        entry.version = 1;
        entry.created_at_ms = now;
        entry.expires_at_ms = expires_at;
        store_[key] = std::move(entry);
    }
    return true;
}

std::optional<std::string> StorageEngine::get(const std::string& key) {
    int64_t now = current_time_ms();
    bool expired = false;

    {
        std::shared_lock<std::shared_mutex> lock(rw_lock_);
        auto it = store_.find(key);
        if (it != store_.end()) {
            if (it->second.is_expired(now)) {
                expired = true;
            } else {
                return it->second.value;
            }
        }
    }

    if (expired) {
        // Delete expired entry
        std::unique_lock<std::shared_mutex> lock(rw_lock_);
        auto it = store_.find(key);
        if (it != store_.end() && it->second.is_expired(now)) {
            store_.erase(it);
        }
    }

    return std::nullopt;
}

bool StorageEngine::del(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(rw_lock_);
    auto it = store_.find(key);
    if (it != store_.end()) {
        store_.erase(it);
        return true;
    }
    return false;
}

bool StorageEngine::exists(const std::string& key) {
    int64_t now = current_time_ms();
    std::shared_lock<std::shared_mutex> lock(rw_lock_);
    auto it = store_.find(key);
    if (it != store_.end()) {
        return !it->second.is_expired(now);
    }
    return false;
}

std::vector<std::string> StorageEngine::get_keys(const std::string& pattern, size_t limit) {
    int64_t now = current_time_ms();
    std::vector<std::string> keys;
    std::shared_lock<std::shared_mutex> lock(rw_lock_);

    for (const auto& [k, entry] : store_) {
        if (!entry.is_expired(now)) {
            if (pattern == "*" || k.find(pattern) != std::string::npos) {
                keys.push_back(k);
                if (keys.size() >= limit) break;
            }
        }
    }
    return keys;
}

std::vector<std::pair<std::string, std::string>> StorageEngine::get_all_entries(const std::string& pattern, size_t limit) {
    int64_t now = current_time_ms();
    std::vector<std::pair<std::string, std::string>> entries;
    std::shared_lock<std::shared_mutex> lock(rw_lock_);

    for (const auto& [k, entry] : store_) {
        if (!entry.is_expired(now)) {
            if (pattern == "*" || k.find(pattern) != std::string::npos) {
                entries.emplace_back(k, entry.value);
                if (entries.size() >= limit) break;
            }
        }
    }
    return entries;
}

uint64_t StorageEngine::append_log_entry(uint64_t term, LogOpType op, const std::string& key, const std::string& value, int64_t ttl_sec) {
    std::lock_guard<std::mutex> lock(log_lock_);
    last_log_index_++;
    last_log_term_ = term;

    LogEntry entry;
    entry.index = last_log_index_;
    entry.term = term;
    entry.op = op;
    entry.key = key;
    entry.value = value;
    entry.ttl_sec = ttl_sec;

    log_.push_back(entry);
    return last_log_index_;
}

bool StorageEngine::apply_log_entry(const LogEntry& entry) {
    if (entry.op == LogOpType::PUT) {
        put(entry.key, entry.value, entry.ttl_sec);
    } else if (entry.op == LogOpType::DEL) {
        del(entry.key);
    }

    std::lock_guard<std::mutex> lock(log_lock_);
    if (entry.index > last_log_index_) {
        last_log_index_ = entry.index;
        last_log_term_ = entry.term;
        log_.push_back(entry);
    }
    return true;
}

std::vector<LogEntry> StorageEngine::get_log_entries_from(uint64_t from_index) const {
    std::lock_guard<std::mutex> lock(log_lock_);
    std::vector<LogEntry> result;
    for (const auto& entry : log_) {
        if (entry.index >= from_index) {
            result.push_back(entry);
        }
    }
    return result;
}

uint64_t StorageEngine::get_last_log_index() const {
    std::lock_guard<std::mutex> lock(log_lock_);
    return last_log_index_;
}

uint64_t StorageEngine::get_last_log_term() const {
    std::lock_guard<std::mutex> lock(log_lock_);
    return last_log_term_;
}

size_t StorageEngine::purge_expired_keys() {
    int64_t now = current_time_ms();
    std::vector<std::string> to_remove;

    {
        std::shared_lock<std::shared_mutex> lock(rw_lock_);
        for (const auto& [k, entry] : store_) {
            if (entry.is_expired(now)) {
                to_remove.push_back(k);
            }
        }
    }

    if (to_remove.empty()) return 0;

    {
        std::unique_lock<std::shared_mutex> lock(rw_lock_);
        for (const auto& k : to_remove) {
            store_.erase(k);
        }
    }
    return to_remove.size();
}

StorageStats StorageEngine::get_stats() {
    int64_t now = current_time_ms();
    StorageStats stats;

    {
        std::shared_lock<std::shared_mutex> lock(rw_lock_);
        stats.total_keys = 0;
        stats.memory_bytes = sizeof(*this);
        for (const auto& [k, entry] : store_) {
            if (!entry.is_expired(now)) {
                stats.total_keys++;
                stats.memory_bytes += k.capacity() + entry.value.capacity() + sizeof(StorageEntry);
            }
        }
    }

    {
        std::lock_guard<std::mutex> llock(log_lock_);
        stats.log_size = log_.size();
    }

    return stats;
}

void StorageEngine::reset() {
    {
        std::unique_lock<std::shared_mutex> lock(rw_lock_);
        store_.clear();
    }
    {
        std::lock_guard<std::mutex> llock(log_lock_);
        log_.clear();
        last_log_index_ = 0;
        last_log_term_ = 0;
    }
}

bool StorageEngine::save_snapshot(const std::string& filepath) {
    std::string tmp_path = filepath + ".tmp";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[StorageEngine] Failed to create snapshot temp file: " << tmp_path << "\n";
        return false;
    }

    int64_t now = current_time_ms();

    std::shared_lock<std::shared_mutex> lock(rw_lock_);

    // 1. Write Magic Header (8 bytes)
    out.write(SNAPSHOT_MAGIC, 8);

    // 2. Count active non-expired keys
    std::vector<std::pair<std::string, StorageEntry>> active_entries;
    for (const auto& [k, entry] : store_) {
        if (!entry.is_expired(now)) {
            active_entries.push_back({k, entry});
        }
    }

    uint64_t entry_count = active_entries.size();
    out.write(reinterpret_cast<const char*>(&entry_count), sizeof(entry_count));

    // 3. Write each entry
    for (const auto& [k, entry] : active_entries) {
        uint32_t k_len = static_cast<uint32_t>(k.size());
        out.write(reinterpret_cast<const char*>(&k_len), sizeof(k_len));
        out.write(k.data(), k_len);

        uint32_t v_len = static_cast<uint32_t>(entry.value.size());
        out.write(reinterpret_cast<const char*>(&v_len), sizeof(v_len));
        out.write(entry.value.data(), v_len);

        out.write(reinterpret_cast<const char*>(&entry.created_at_ms), sizeof(entry.created_at_ms));
        out.write(reinterpret_cast<const char*>(&entry.expires_at_ms), sizeof(entry.expires_at_ms));
        out.write(reinterpret_cast<const char*>(&entry.version), sizeof(entry.version));
    }

    out.flush();
    out.close();

    // 4. Atomic rename
    if (std::rename(tmp_path.c_str(), filepath.c_str()) != 0) {
        std::cerr << "[StorageEngine] Failed to rename snapshot from " << tmp_path << " to " << filepath << "\n";
        return false;
    }

    return true;
}

bool StorageEngine::load_snapshot(const std::string& filepath) {
    std::ifstream in(filepath, std::ios::binary);
    if (!in.is_open()) {
        return false; // Snapshot does not exist yet
    }

    char magic[8];
    in.read(magic, 8);
    if (std::memcmp(magic, SNAPSHOT_MAGIC, 8) != 0) {
        std::cerr << "[StorageEngine] Corrupted snapshot magic header!\n";
        return false;
    }

    uint64_t entry_count = 0;
    in.read(reinterpret_cast<char*>(&entry_count), sizeof(entry_count));

    int64_t now = current_time_ms();
    std::unordered_map<std::string, StorageEntry> loaded_store;

    for (uint64_t i = 0; i < entry_count; i++) {
        uint32_t k_len = 0;
        in.read(reinterpret_cast<char*>(&k_len), sizeof(k_len));
        std::string key(k_len, '\0');
        in.read(&key[0], k_len);

        uint32_t v_len = 0;
        in.read(reinterpret_cast<char*>(&v_len), sizeof(v_len));
        std::string value(v_len, '\0');
        in.read(&value[0], v_len);

        StorageEntry entry;
        entry.value = std::move(value);
        in.read(reinterpret_cast<char*>(&entry.created_at_ms), sizeof(entry.created_at_ms));
        in.read(reinterpret_cast<char*>(&entry.expires_at_ms), sizeof(entry.expires_at_ms));
        in.read(reinterpret_cast<char*>(&entry.version), sizeof(entry.version));

        if (!entry.is_expired(now)) {
            loaded_store[key] = std::move(entry);
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(rw_lock_);
        store_ = std::move(loaded_store);
    }

    return true;
}

} // namespace distrikv
