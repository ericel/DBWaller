#pragma once

#include "dbwaller/adapters/adapter.hpp"

#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace dbwaller::policy {

/**
 * CacheKeyBuilder:
 * Creates canonical cache keys with explicit namespaces and vary-by dimensions.
 *
 * Example output:
 *   v1|ns=post|get|id=123|tenant=dev|viewer=42|loc=en
 */
class CacheKeyBuilder {
public:
    CacheKeyBuilder& version(std::string_view v) { version_ = v; return *this; }
    CacheKeyBuilder& ns(std::string_view n) { ns_ = n; return *this; }
    CacheKeyBuilder& op(std::string_view o) { op_ = o; return *this; }

    CacheKeyBuilder& id(std::string_view id) { id_ = id; return *this; }

    // Add arbitrary key=value dimensions (already sanitized/encoded by caller)
    CacheKeyBuilder& dim(std::string_view k, std::string_view v) {
        dims_.push_back({std::string(k), std::string(v)});
        return *this;
    }

    // Common vary-by dimensions
    CacheKeyBuilder& vary_tenant(const dbwaller::adapters::RequestContext& ctx) {
        if (!ctx.tenant.empty()) dim("tenant", ctx.tenant);
        return *this;
    }

    CacheKeyBuilder& vary_viewer(const dbwaller::adapters::RequestContext& ctx) {
        // viewer is security-sensitive. If viewer_id is empty, treat as "anon".
        dim("viewer", ctx.viewer_id.empty() ? "anon" : ctx.viewer_id);
        return *this;
    }

    CacheKeyBuilder& vary_locale(const dbwaller::adapters::RequestContext& ctx) {
        if (!ctx.locale.empty()) dim("loc", ctx.locale);
        return *this;
    }

    // Finalize
    std::string build() const {
        std::ostringstream oss;
        oss << (version_.empty() ? "v1" : version_);
        oss << "|ns=" << ns_;
        if (!op_.empty()) oss << "|op=" << op_;
        if (!id_.empty()) oss << "|id=" << id_;

        for (const auto& kv : dims_) {
            oss << "|" << kv.first << "=" << kv.second;
        }
        return oss.str();
    }

private:
    std::string version_ = "v1";
    std::string ns_;
    std::string op_;
    std::string id_;

    std::vector<std::pair<std::string, std::string>> dims_;
};

} // namespace dbwaller::policy
