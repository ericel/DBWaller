#pragma once

#include "dbwaller/core/sharded_engine.hpp"

#include <chrono>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace dbwaller::daemon {

// Internal server facade, NOT the public remote SDK. A future transport must
// authenticate/authorize these fields; they must never be trusted request data.
struct AccessContext {
    std::string principal;
    std::string cache_namespace;
};

enum class ErrorCode { InvalidArgument, PermissionDenied, ResourceExhausted, Unavailable, Internal };
class ServiceError : public std::runtime_error {
public:
    ServiceError(ErrorCode code, const char* message) : std::runtime_error(message), code_(code) {}
    ErrorCode code() const noexcept { return code_; }
private:
    ErrorCode code_;
};

enum class Publication { Stored, Invalidated, Expired, Consumed, UnknownTicket };

struct LoadGrant {
    std::string ticket; // Opaque binary bytes; never log this capability.
    uint64_t valid_for_ms;
};
struct LookupResult {
    core::ShardedEngine::Snapshot snapshot;
    std::optional<LoadGrant> load;
};
struct PublishOptions {
    uint64_t ttl_ms = 1000;
    uint64_t swr_ms = 0;
    std::vector<std::string> tags;
};

class CacheService {
public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;
    struct Limits {
        size_t max_ticket_records = 4096; // Includes consumed receipts until expiry.
        size_t max_ticket_key_bytes = 4 * 1024 * 1024; // Encoded keys held by active tickets.
        size_t reap_per_call = 64;
        uint64_t ticket_lifetime_ms = 30'000;
        size_t max_identity_bytes = 256;
        size_t max_key_bytes = 4096;
        size_t max_value_bytes = 1024 * 1024;
        size_t max_tags = 32;
        size_t max_tag_bytes = 256;
        uint64_t max_cache_lifetime_ms = 86'400'000; // TTL + SWR, at most one day.
    };
    struct Stats {
        bool ready;
        size_t ticket_records;
        size_t ticket_key_bytes;
        core::EngineStats engine;
    };

    CacheService(core::ShardedEngine::Config config, Limits limits,
                 Now now = [] { return Clock::now(); });
    // Callers must finish before destruction. The injected monotonic clock must
    // not block or reenter; it is invoked under the service mutex.
    LookupResult lookup(const AccessContext&, const std::string& key, bool acquire_ticket = false);
    LoadGrant begin_load(const AccessContext&, const std::string& key);
    Publication publish(const AccessContext&, const std::string& ticket,
                        std::string value, const PublishOptions&);
    void invalidate_key(const AccessContext&, const std::string& key);
    void invalidate_tag(const AccessContext&, const std::string& tag);
    // The transport must restrict diagnostics to an operator identity.
    Stats stats();
    const std::string& instance_id() const noexcept { return instance_id_; }
    void shutdown();

private:
    struct Record {
        std::string id;
        AccessContext owner;
        Clock::time_point expires;
        std::optional<core::LoadTicket> load;
        size_t key_bytes;
    };
    using Records = std::list<Record>;
    void require_open() const;
    void validate_context(const AccessContext&) const;
    void validate_key(const std::string&) const;
    void validate_tag(const std::string&) const;
    core::PutOptions put_options(const AccessContext&, const std::string&, const PublishOptions&) const;
    static std::string scope(const std::string& ns, const std::string& bytes);
    void erase(Records::iterator);
    void reap(Clock::time_point);
    LoadGrant retain(const AccessContext&, core::LoadTicket, size_t key_bytes, Clock::time_point);
    void reserve_ticket(size_t key_bytes, Clock::time_point);

    const Limits limits_;
    const Now now_;
    const std::string instance_id_;
    core::ShardedEngine engine_;
    std::mutex mu_;
    bool closed_ = false;
    uint64_t sequence_ = 0;
    size_t ticket_key_bytes_ = 0;
    Records records_; // Fixed lifetime + monotonic admissions => expiry order.
    std::unordered_map<std::string, Records::iterator> tickets_;
};
} // namespace dbwaller::daemon
