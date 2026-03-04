#include <catch2/catch_test_macros.hpp>

#include "dbwaller/adapters/adapter.hpp"
#include "dbwaller/policy/rules.hpp"

TEST_CASE("PolicyRuleSet decides key dimensions and clamps ttl") {
    dbwaller::policy::PolicyRuleSet rules;
    dbwaller::policy::Rule rule;
    rule.scope = dbwaller::policy::CacheScope::PerViewer;
    rule.ttl_ms = 10;
    rule.min_ttl_ms = 50;
    rule.max_ttl_ms = 1'000;
    rule.extra_tags = {"group:vip_users"};
    rules.set_rule("post", rule);

    dbwaller::adapters::RequestContext ctx;
    ctx.viewer_id = "alice";
    ctx.tenant = "tenantA";
    ctx.locale = "en";

    const auto decision = rules.decide("post", "123", "get", ctx);

    REQUIRE(decision.cacheable);
    REQUIRE(decision.ttl_ms == 50);
    REQUIRE(decision.cache_key == "v1|ns=post|op=get|id=123|tenant=tenantA|viewer=alice|loc=en");
    REQUIRE(decision.tags.size() == 2);
    REQUIRE(decision.tags[0] == "post:123");
    REQUIRE(decision.tags[1] == "group:vip_users");
}

TEST_CASE("PolicyRuleSet fail-closed behavior when claims fingerprint is required") {
    dbwaller::policy::PolicyRuleSet rules;
    dbwaller::policy::Rule rule;
    rule.scope = dbwaller::policy::CacheScope::PerViewer;
    rule.vary_by_claims = true;
    rule.require_claims_fingerprint = true;
    rules.set_rule("adminview", rule);

    dbwaller::adapters::RequestContext ctx_missing_claims;
    ctx_missing_claims.viewer_id = "bob";

    const auto missing = rules.decide("adminview", "dashboard", "get", ctx_missing_claims);
    REQUIRE_FALSE(missing.cacheable);
    REQUIRE(missing.cache_key == "v1|ns=adminview|op=get|id=dashboard|viewer=bob");

    dbwaller::adapters::RequestContext ctx_with_claims;
    ctx_with_claims.viewer_id = "bob";
    ctx_with_claims.claims_fingerprint = "abc123";

    const auto present = rules.decide("adminview", "dashboard", "get", ctx_with_claims);
    REQUIRE(present.cacheable);
    REQUIRE(present.cache_key == "v1|ns=adminview|op=get|id=dashboard|viewer=bob|claims=abc123");
}
