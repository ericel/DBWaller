#include <catch2/catch_test_macros.hpp>

#include "dbwaller/adapters/request_context.hpp"

TEST_CASE("set_claims_fingerprint writes deterministic value to context") {
    dbwaller::adapters::RequestContext ctx_a;
    dbwaller::adapters::RequestContext ctx_b;

    dbwaller::adapters::set_claims_fingerprint(ctx_a, {"admin", "user"}, {"read", "write"});
    dbwaller::adapters::set_claims_fingerprint(ctx_b, {"user", "admin", "user"}, {"write", "read"});

    REQUIRE_FALSE(ctx_a.claims_fingerprint.empty());
    REQUIRE(ctx_a.claims_fingerprint == ctx_b.claims_fingerprint);
    REQUIRE(ctx_a.claims_fingerprint.size() == 16);
}

TEST_CASE("set_claims_fingerprint supports configurable hex length") {
    dbwaller::adapters::RequestContext ctx;
    dbwaller::adapters::set_claims_fingerprint(ctx, {"user"}, {"read"}, {}, 24);

    REQUIRE(ctx.claims_fingerprint.size() == 24);
}

TEST_CASE("set_claims_fingerprint changes when claims change") {
    dbwaller::adapters::RequestContext base;
    dbwaller::adapters::RequestContext changed;

    dbwaller::adapters::set_claims_fingerprint(base, {"user"}, {"read"});
    dbwaller::adapters::set_claims_fingerprint(changed, {"admin"}, {"read"});

    REQUIRE(base.claims_fingerprint != changed.claims_fingerprint);
}
