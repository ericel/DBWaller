#include "dbwaller/daemon/cache_service.hpp"

#include <openssl/rand.h>

#include <limits>

namespace dbwaller::daemon {
namespace {
std::string random_bytes(int count) {
    std::string result(static_cast<size_t>(count), '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(result.data()), count) != 1) {
        throw ServiceError(ErrorCode::Internal, "ticket entropy unavailable");
    }
    return result;
}
void append_u64(std::string& target, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) target.push_back(static_cast<char>(value >> shift));
}
CacheService::Limits validate_limits(CacheService::Limits limits) {
    if (!limits.max_ticket_records || !limits.max_ticket_key_bytes || !limits.reap_per_call ||
        !limits.ticket_lifetime_ms || limits.ticket_lifetime_ms > 86'400'000 ||
        !limits.max_identity_bytes || limits.max_identity_bytes > 4096 ||
        !limits.max_key_bytes || limits.max_key_bytes > 1024 * 1024 ||
        !limits.max_value_bytes || limits.max_value_bytes > std::numeric_limits<uint32_t>::max() ||
        !limits.max_tag_bytes || limits.max_tag_bytes > 1024 * 1024 || limits.max_tags > 1024 ||
        !limits.max_cache_lifetime_ms || limits.max_cache_lifetime_ms > 86'400'000) {
        throw ServiceError(ErrorCode::InvalidArgument, "invalid service limits");
    }
    return limits;
}
core::ShardedEngine::Config cache_only(core::ShardedEngine::Config config) {
    config.enable_compute_pool = false; // Backend callbacks never run in this service.
    return config;
}
} // namespace

CacheService::CacheService(core::ShardedEngine::Config config, Limits limits, Now now)
    : limits_(validate_limits(limits)), now_(std::move(now)), instance_id_(random_bytes(16)),
      engine_(cache_only(config)) {
    if (!now_) throw ServiceError(ErrorCode::InvalidArgument, "missing monotonic clock");
}

void CacheService::require_open() const {
    if (closed_) throw ServiceError(ErrorCode::Unavailable, "cache service stopped");
}
void CacheService::validate_context(const AccessContext& ctx) const {
    if (ctx.principal.empty() || ctx.cache_namespace.empty() ||
        ctx.principal.size() > limits_.max_identity_bytes ||
        ctx.cache_namespace.size() > limits_.max_identity_bytes) {
        throw ServiceError(ErrorCode::PermissionDenied, "invalid authorized context");
    }
}
void CacheService::validate_key(const std::string& key) const {
    if (key.empty() || key.size() > limits_.max_key_bytes)
        throw ServiceError(ErrorCode::InvalidArgument, "invalid key size");
}
void CacheService::validate_tag(const std::string& tag) const {
    if (tag.empty() || tag.size() > limits_.max_tag_bytes)
        throw ServiceError(ErrorCode::InvalidArgument, "invalid tag size");
}
std::string CacheService::scope(const std::string& ns, const std::string& bytes) {
    std::string encoded;
    append_u64(encoded, ns.size());
    encoded += ns;
    encoded += bytes;
    return encoded;
}
core::PutOptions CacheService::put_options(const AccessContext& ctx, const std::string& value,
                                          const PublishOptions& options) const {
    if (value.size() > limits_.max_value_bytes || options.tags.size() > limits_.max_tags ||
        options.ttl_ms > limits_.max_cache_lifetime_ms ||
        options.swr_ms > limits_.max_cache_lifetime_ms - options.ttl_ms) {
        throw ServiceError(ErrorCode::InvalidArgument, "publication exceeds limits");
    }
    core::PutOptions result;
    result.ttl_ms = options.ttl_ms;
    result.swr_ms = options.swr_ms;
    for (const auto& tag : options.tags) {
        validate_tag(tag);
        result.tags.push_back(scope(ctx.cache_namespace, tag));
    }
    return result;
}

void CacheService::erase(Records::iterator it) {
    ticket_key_bytes_ -= it->key_bytes;
    tickets_.erase(it->id);
    records_.erase(it);
}
void CacheService::reap(Clock::time_point now) {
    for (size_t i = 0; i < limits_.reap_per_call && !records_.empty(); ++i) {
        if (now < records_.front().expires) break;
        erase(records_.begin());
    }
}
void CacheService::reserve_ticket(size_t key_bytes, Clock::time_point now) {
    reap(now);
    if (records_.size() >= limits_.max_ticket_records ||
        key_bytes > limits_.max_ticket_key_bytes - ticket_key_bytes_ ||
        sequence_ == std::numeric_limits<uint64_t>::max()) {
        throw ServiceError(ErrorCode::ResourceExhausted, "ticket capacity exhausted");
    }
}
LoadGrant CacheService::retain(const AccessContext& ctx, core::LoadTicket load,
                              size_t key_bytes, Clock::time_point now) {
    auto id = instance_id_;
    append_u64(id, ++sequence_); // Never reused, including after expired records are reaped.
    id += random_bytes(32);
    const auto lifetime = std::chrono::milliseconds(limits_.ticket_lifetime_ms);
    if (now > Clock::time_point::max() - lifetime)
        throw ServiceError(ErrorCode::Internal, "monotonic clock out of range");
    records_.push_back({id, ctx, now + lifetime, std::move(load), key_bytes});
    try {
        tickets_.emplace(id, std::prev(records_.end()));
    } catch (...) {
        records_.pop_back();
        throw;
    }
    ticket_key_bytes_ += key_bytes;
    return {std::move(id), limits_.ticket_lifetime_ms};
}

LookupResult CacheService::lookup(const AccessContext& ctx, const std::string& key, bool acquire_ticket) {
    validate_context(ctx);
    validate_key(key);
    const auto encoded = scope(ctx.cache_namespace, key);
    std::lock_guard lock(mu_);
    require_open();
    const auto now = now_();
    if (!acquire_ticket) {
        reap(now);
        return {engine_.get_snapshot(encoded), std::nullopt};
    }
    reserve_ticket(encoded.size(), now);
    auto [snapshot, load] = engine_.get_snapshot_and_begin_load(encoded);
    auto grant = retain(ctx, std::move(load), encoded.size(), now);
    return {std::move(snapshot), std::move(grant)};
}
LoadGrant CacheService::begin_load(const AccessContext& ctx, const std::string& key) {
    validate_context(ctx);
    validate_key(key);
    const auto encoded = scope(ctx.cache_namespace, key);
    std::lock_guard lock(mu_);
    require_open();
    const auto now = now_();
    reserve_ticket(encoded.size(), now);
    return retain(ctx, engine_.begin_load(encoded), encoded.size(), now);
}
Publication CacheService::publish(const AccessContext& ctx, const std::string& ticket,
                                  std::string value, const PublishOptions& options) {
    validate_context(ctx);
    if (ticket.size() != 56) throw ServiceError(ErrorCode::InvalidArgument, "invalid ticket size");
    auto put = put_options(ctx, value, options); // Reject bad input without consuming the ticket.
    std::lock_guard lock(mu_);
    require_open();
    const auto now = now_();
    auto found = tickets_.find(ticket);
    if (found == tickets_.end()) {
        reap(now);
        return Publication::UnknownTicket;
    }
    auto it = found->second;
    if (it->owner.principal != ctx.principal || it->owner.cache_namespace != ctx.cache_namespace)
        throw ServiceError(ErrorCode::PermissionDenied, "ticket belongs to another context");
    if (now >= it->expires) {
        erase(it);
        reap(now);
        return Publication::Expired;
    }
    reap(now); // Cannot remove this unexpired record.
    if (!it->load) return Publication::Consumed;
    auto load = std::move(*it->load);
    it->load.reset(); // Consume before the mutation, including on an exception.
    ticket_key_bytes_ -= it->key_bytes;
    it->key_bytes = 0;
    return engine_.put_if_current(load, std::move(value), put) == core::PublishStatus::Stored
        ? Publication::Stored : Publication::Invalidated;
}
void CacheService::invalidate_key(const AccessContext& ctx, const std::string& key) {
    validate_context(ctx);
    validate_key(key);
    std::lock_guard lock(mu_);
    require_open();
    reap(now_());
    engine_.invalidate_key(scope(ctx.cache_namespace, key));
}
void CacheService::invalidate_tag(const AccessContext& ctx, const std::string& tag) {
    validate_context(ctx);
    validate_tag(tag);
    std::lock_guard lock(mu_);
    require_open();
    reap(now_());
    engine_.invalidate_tag(scope(ctx.cache_namespace, tag));
}
CacheService::Stats CacheService::stats() {
    std::lock_guard lock(mu_);
    reap(now_());
    return {!closed_, records_.size(), ticket_key_bytes_, engine_.stats_snapshot()};
}
void CacheService::shutdown() {
    std::lock_guard lock(mu_);
    closed_ = true;
    tickets_.clear();
    records_.clear();
    ticket_key_bytes_ = 0;
    engine_.shutdown();
}
} // namespace dbwaller::daemon
