#pragma once

#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbwaller::security {

/**
 * Normalize a list of strings:
 * - trims empty items
 * - sorts
 * - de-duplicates
 */
std::vector<std::string> normalize_list(std::vector<std::string> items);

/**
 * Build a stable "claims string" from:
 * - roles
 * - scopes
 * - extra claims (key/value)
 *
 * Output is deterministic: keys sorted, values sorted when provided as lists, etc.
 *
 * Example:
 *   roles=admin,user;scopes=read,write;kv.department=finance;kv.tier=premium
 */
std::string normalize_claims_string(
    const std::vector<std::string>& roles,
    const std::vector<std::string>& scopes,
    const std::map<std::string, std::string>& extra_kv = {}
);

/**
 * Make a compact fingerprint:
 * - sha256_hex(normalized_claims_string)
 * - optionally truncate to keep cache keys shorter
 *
 * truncated_hex_chars:
 *   0 => full 64 hex chars
 *   16 => 64-bit-ish (not cryptographic, but plenty for cache partitioning)
 */
std::string claims_fingerprint_hex(
    const std::vector<std::string>& roles,
    const std::vector<std::string>& scopes,
    const std::map<std::string, std::string>& extra_kv = {},
    size_t truncated_hex_chars = 16
);

} // namespace dbwaller::security
