#pragma once

#include "dbwaller/daemon/cache_service.hpp"

#include <array>
#include <filesystem>
#include <set>

namespace dbwaller::daemon {
struct Credential {
    std::string principal;
    std::array<unsigned char, 32> token_hash{};
    std::set<std::string> namespaces;
    bool write = false;
    bool invalidate = false;
    bool admin = false;
};
struct ServerConfig {
    std::string address = "127.0.0.1:7447";
    bool insecure_loopback = false;
    std::string certificate; // PEM loaded from files, never logged.
    std::string private_key;
    std::vector<Credential> credentials;
    core::ShardedEngine::Config cache;
    CacheService::Limits limits;
    size_t workers = 4;
    size_t queue_capacity = 64;
    size_t max_active_rpcs = 128;
    int max_connections = 64;
    int max_streams_per_connection = 16;
    int max_message_bytes = 2 * 1024 * 1024;
    size_t transport_memory_bytes = 64 * 1024 * 1024;
    uint64_t shutdown_grace_ms = 5000;
    ServerConfig() { cache.enable_compute_pool = false; cache.max_entries_total = 16'384; }
};

ServerConfig load_server_config(const std::filesystem::path& path);
void validate_server_config(const ServerConfig&);
std::array<unsigned char, 32> token_digest(const std::string& token);
std::string read_bounded_file(const std::filesystem::path& path, size_t max_bytes);
} // namespace dbwaller::daemon
