#include <catch2/catch_test_macros.hpp>

#include "dbwaller/adapters/adapter.hpp"
#include "dbwaller/policy/cache_key.hpp"

TEST_CASE("CacheKeyBuilder uses default version and required fields") {
    const auto key = dbwaller::policy::CacheKeyBuilder{}
        .ns("post")
        .op("get")
        .id("42")
        .build();

    REQUIRE(key == "v1|ns=post|op=get|id=42");
}

TEST_CASE("CacheKeyBuilder preserves dimension insertion order") {
    const auto key = dbwaller::policy::CacheKeyBuilder{}
        .version("v2")
        .ns("feed")
        .op("list")
        .id("home")
        .dim("tenant", "t1")
        .dim("viewer", "u7")
        .dim("loc", "en")
        .build();

    REQUIRE(key == "v2|ns=feed|op=list|id=home|tenant=t1|viewer=u7|loc=en");
}

TEST_CASE("CacheKeyBuilder vary helpers apply expected context behavior") {
    dbwaller::adapters::RequestContext ctx;
    ctx.tenant = "acme";
    ctx.locale = "fr";

    const auto anon_key = dbwaller::policy::CacheKeyBuilder{}
        .ns("timeline")
        .op("get")
        .id("global")
        .vary_tenant(ctx)
        .vary_viewer(ctx)
        .vary_locale(ctx)
        .build();

    REQUIRE(anon_key == "v1|ns=timeline|op=get|id=global|tenant=acme|viewer=anon|loc=fr");

    ctx.viewer_id = "viewer123";
    const auto viewer_key = dbwaller::policy::CacheKeyBuilder{}
        .ns("timeline")
        .op("get")
        .id("global")
        .vary_tenant(ctx)
        .vary_viewer(ctx)
        .vary_locale(ctx)
        .build();

    REQUIRE(viewer_key == "v1|ns=timeline|op=get|id=global|tenant=acme|viewer=viewer123|loc=fr");
}
