#pragma once
// Example application policy, not part of the SDK's public API. These blocking
// helpers run on main/a backend worker, never on an application's event loop.
#include <dbwaller/remote/client.hpp>
#include <algorithm>
#ifdef DBWALLER_EXAMPLE_EMBEDDED
#include <dbwaller/core/sharded_engine.hpp>
#endif

namespace example {
namespace r = dbwaller::remote;
enum class Publication { Stored, Fenced, Unavailable };
struct Policy {
    r::Milliseconds budget{2000}, cache_budget{250}, ttl{60'000}, swr{0};
    bool allow_stale = false;
    unsigned max_backend_fetches = 2;
};
struct DisabledStore {
    struct Ticket {};
    std::optional<std::string> lookup(const std::string&, bool, r::Clock::time_point) { return {}; }
    std::optional<Ticket> begin(const std::string&, r::Clock::time_point) { return {}; }
    Publication publish(const Ticket&, const std::string&, const Policy&, r::Clock::time_point) { return Publication::Unavailable; }
    bool invalidate(const std::string&) { return true; }
};
struct RemoteStore {
    using Ticket = r::Ticket;
    r::Client client;
    explicit RemoteStore(r::ClientOptions options) : client(std::move(options)) {}
    std::optional<std::string> lookup(const std::string& key, bool allow_stale, r::Clock::time_point deadline) {
        auto result = client.lookup(key, false, {deadline}).get();
        if (!result) return {};
        const auto& hit = result.value();
        if ((hit.kind == r::LookupKind::Fresh && hit.fresh_for.count() > 0) ||
            (allow_stale && hit.kind == r::LookupKind::Stale && hit.servable_for.count() > 0)) return hit.value;
        return {};
    }
    std::optional<Ticket> begin(const std::string& key, r::Clock::time_point deadline) {
        auto result = client.begin_load(key, {deadline}).get();
        if (!result) return {};
        return result.value().ticket;
    }
    Publication publish(const Ticket& ticket, const std::string& value, const Policy& policy, r::Clock::time_point deadline) {
        auto result = client.publish(ticket, value, {policy.ttl, policy.swr, {"example"}}, {deadline}).get();
        if (!result) return Publication::Unavailable;
        return result.value() == r::PublishStatus::Stored ? Publication::Stored : Publication::Fenced;
    }
    bool invalidate(const std::string& key) { return client.invalidate_key(key).get().has_value(); }
};
#ifdef DBWALLER_EXAMPLE_EMBEDDED
struct EmbeddedStore {
    using Ticket = dbwaller::core::LoadTicket; // Native and remote tickets are separate types.
    dbwaller::core::ShardedEngine engine;
    EmbeddedStore() : engine([] { dbwaller::core::ShardedEngine::Config c; c.enable_compute_pool = false; return c; }()) {}
    std::optional<std::string> lookup(const std::string& key, bool allow_stale, r::Clock::time_point) {
        auto snapshot = engine.get_snapshot(key);
        using Kind = dbwaller::core::ShardedEngine::GetKind;
        if (snapshot.result.kind == Kind::Fresh || (allow_stale && snapshot.result.kind == Kind::Stale)) return snapshot.result.value;
        return {};
    }
    std::optional<Ticket> begin(const std::string& key, r::Clock::time_point) { return engine.begin_load(key); }
    Publication publish(const Ticket& ticket, const std::string& value, const Policy& policy, r::Clock::time_point) {
        dbwaller::core::PutOptions p; p.ttl_ms = policy.ttl.count(); p.swr_ms = policy.swr.count(); p.tags = {"example"};
        return engine.put_if_current(ticket, value, p) == dbwaller::core::PublishStatus::Stored ? Publication::Stored : Publication::Fenced;
    }
    bool invalidate(const std::string& key) { engine.invalidate_key(key); return true; }
};
#endif

template<class Store, class Backend>
std::string get(Store& store, const std::string& key, Backend backend, Policy policy = {}) {
    if (policy.budget.count() <= 0 || policy.budget > std::chrono::hours(24) ||
        policy.cache_budget.count() <= 0 || policy.cache_budget >= policy.budget ||
        policy.max_backend_fetches < 1 || policy.max_backend_fetches > 2 ||
        policy.ttl.count() < 0 || policy.swr.count() < 0 || policy.ttl > std::chrono::hours(24) ||
        policy.swr > std::chrono::hours(24) || policy.ttl + policy.swr > std::chrono::hours(24))
        throw std::invalid_argument("invalid example cache policy");
    const auto deadline = r::Clock::now() + policy.budget;
    const auto check_budget = [&] { if (r::Clock::now() >= deadline) throw std::runtime_error("application budget expired"); };
    const auto cache_deadline = [&] { return std::min(deadline, r::Clock::now() + policy.cache_budget); };
    if (auto hit = store.lookup(key, policy.allow_stale, cache_deadline())) { check_budget(); return *hit; }
    for (unsigned attempt = 0; attempt < policy.max_backend_fetches; ++attempt) {
        check_budget();
        auto ticket = store.begin(key, cache_deadline()); // Must happen before the backend fetch.
        check_budget();
        auto value = backend(deadline); // Backend owns authorization and must honor this budget.
        check_budget();
        if (!ticket) return value; // Explicit backend fallback; no implicit local cache.
        const auto published = store.publish(*ticket, value, policy, cache_deadline());
        check_budget();
        if (published == Publication::Stored) return value;
        if (published == Publication::Unavailable) {
            // Unknown publication: do not replay or give old bytes a new ticket.
            // This example chooses one fresh backend read without caching.
            if (attempt + 1 >= policy.max_backend_fetches) break;
            auto fallback = backend(deadline); check_budget(); return fallback;
        }
        // Explicitly fenced/expired/consumed/unknown ticket: discard the old value,
        // start a new load, and fetch again within the bounded application policy.
    }
    throw std::runtime_error("cache load invalidated; backend retry budget exhausted");
}
} // namespace example
