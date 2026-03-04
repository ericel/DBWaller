#include <catch2/catch_test_macros.hpp>

#include "dbwaller/security/claims.hpp"

TEST_CASE("normalize_list removes empties, sorts, and deduplicates") {
    std::vector<std::string> values = {"writer", "", "admin", "writer", "reader", ""};

    const auto normalized = dbwaller::security::normalize_list(values);

    REQUIRE(normalized.size() == 3);
    REQUIRE(normalized[0] == "admin");
    REQUIRE(normalized[1] == "reader");
    REQUIRE(normalized[2] == "writer");
}

TEST_CASE("normalize_claims_string is deterministic and skips empty kv") {
    const std::vector<std::string> roles = {"user", "admin", "user"};
    const std::vector<std::string> scopes = {"write", "read"};
    const std::map<std::string, std::string> extra = {
        {"department", "finance"},
        {"skip_empty_value", ""},
        {"tier", "premium"}
    };

    const auto normalized = dbwaller::security::normalize_claims_string(roles, scopes, extra);

    REQUIRE(normalized == "roles=admin,user;scopes=read,write;kv.department=finance;kv.tier=premium");
}

TEST_CASE("claims_fingerprint_hex is stable and supports truncation") {
    const std::vector<std::string> roles_a = {"admin", "user"};
    const std::vector<std::string> scopes_a = {"read", "write"};

    const std::vector<std::string> roles_b = {"user", "admin", "user"};
    const std::vector<std::string> scopes_b = {"write", "read"};

    const auto full_a = dbwaller::security::claims_fingerprint_hex(roles_a, scopes_a, {}, 0);
    const auto full_b = dbwaller::security::claims_fingerprint_hex(roles_b, scopes_b, {}, 0);
    const auto short_a = dbwaller::security::claims_fingerprint_hex(roles_a, scopes_a, {}, 16);

    REQUIRE(full_a == full_b);
    REQUIRE(full_a.size() == 64);
    REQUIRE(short_a.size() == 16);
    REQUIRE(full_a.rfind(short_a, 0) == 0);
}
