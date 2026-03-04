#pragma once

#include "dbwaller/adapters/adapter.hpp"
#include "dbwaller/security/claims.hpp"

#include <map>
#include <string>
#include <vector>

namespace dbwaller::adapters {

/**
 * Helper API for setting claims fingerprint on RequestContext safely.
 */
inline void set_claims_fingerprint(
    RequestContext& ctx,
    const std::vector<std::string>& roles,
    const std::vector<std::string>& scopes,
    const std::map<std::string, std::string>& extra_kv = {},
    size_t truncated_hex_chars = 16
) {
    ctx.claims_fingerprint = dbwaller::security::claims_fingerprint_hex(
        roles, scopes, extra_kv, truncated_hex_chars
    );
}

} // namespace dbwaller::adapters
