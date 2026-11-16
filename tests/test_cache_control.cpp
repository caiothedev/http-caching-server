#include <catch2/catch_test_macros.hpp>

#include "cache/cache_control.hpp"

using proxy::cache::parse_cache_control;

TEST_CASE("parses boolean directives case-insensitively", "[cache-control]") {
    auto cc = parse_cache_control("No-Cache, no-store, MUST-REVALIDATE, public, private, proxy-revalidate");
    CHECK(cc.no_cache);
    CHECK(cc.no_store);
    CHECK(cc.must_revalidate);
    CHECK(cc.is_public);
    CHECK(cc.is_private);
    CHECK(cc.proxy_revalidate);
}

TEST_CASE("parses numeric directives", "[cache-control]") {
    auto cc = parse_cache_control("max-age=60, s-maxage=\"120\", min-fresh=5, max-stale=10");
    CHECK(cc.max_age == 60);
    CHECK(cc.s_maxage == 120);
    CHECK(cc.min_fresh == 5);
    CHECK(cc.max_stale == 10);
    CHECK_FALSE(cc.max_stale_any);
}

TEST_CASE("max-stale without a value accepts any staleness", "[cache-control]") {
    auto cc = parse_cache_control("max-stale");
    CHECK(cc.max_stale_any);
    CHECK_FALSE(cc.max_stale);
}

TEST_CASE("invalid and huge values", "[cache-control]") {
    CHECK(parse_cache_control("max-age=abc").max_age == 0);
    CHECK(parse_cache_control("max-age=99999999999999999999999").max_age == 2147483647);
}

TEST_CASE("qualified forms and quoted commas", "[cache-control]") {
    auto cc = parse_cache_control("private=\"Set-Cookie, X-Foo\", max-age=30");
    CHECK(cc.is_private);
    CHECK(cc.max_age == 30);
}

TEST_CASE("empty and unknown directives are ignored", "[cache-control]") {
    auto cc = parse_cache_control(" , foo=bar, ,immutable");
    CHECK_FALSE(cc.no_cache);
    CHECK_FALSE(cc.max_age);
}