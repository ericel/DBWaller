#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dbwaller::adapters {

/**
 * RequestContext: passed through DBWaller into adapters so caching can be policy-aware later.
 * Keep it minimal now; we’ll expand with viewer_id, claims, locale, field masks, etc.
 */
struct RequestContext {
    std::string tenant;     // e.g. "prod", "staging", or customer/tenant id
    std::string viewer_id;  // optional; empty means "anonymous/system"
    std::string locale;     // optional; e.g. "en", "es"

    // NEW: hashed representation of authorization context (roles/scopes/claims)
    // Never put raw JWT in cache keys. Always use a fingerprint.
    std::string claims_fingerprint; // e.g. sha256 hex of normalized claims
};

/**
 * FetchResult: authoritative fetch output.
 * - value: bytes you want cached (in DBWaller we treat them as encrypted bytes)
 * - ttl_ms: recommended TTL from the source (or policy)
 * - tags: invalidation tags to associate with this object
 */
struct FetchResult {
    std::string value;
    uint64_t ttl_ms = 1000;
    std::vector<std::string> tags;
};

/**
 * Adapter: contract for authoritative data fetch.
 * DBWaller uses this to fill cache on miss.
 *
 * This is intentionally synchronous for Model A embedded; DBWaller already runs it on its compute pool.
 * Later, we can add async adapters (coroutines/futures) without breaking this API.
 */
class Adapter {
public:
    virtual ~Adapter() = default;

    // Return nullopt if not found or not cacheable.
    virtual std::optional<FetchResult> fetch_one(
        const std::string& key,
        const RequestContext& ctx
    ) = 0;

    // Optional: batch fetch. Default implementation falls back to N calls.
    virtual std::unordered_map<std::string, FetchResult> fetch_batch(
        const std::vector<std::string>& keys,
        const RequestContext& ctx
    ) {
        std::unordered_map<std::string, FetchResult> out;
        out.reserve(keys.size());
        for (const auto& k : keys) {
            if (auto r = fetch_one(k, ctx)) out.emplace(k, std::move(*r));
        }
        return out;
    }
};

} // namespace dbwaller::adapters
