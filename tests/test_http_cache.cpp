#include <catch2/catch_test_macros.hpp>

#include "cache/http_cache.hpp"

using namespace proxy;
using namespace std::chrono_literals;
using cache::Clock;
using cache::HttpCache;
using cache::LookupStatus;

namespace {

    const Clock::time_point kT0 = Clock::time_point{std::chrono::seconds(1'700'000'000)};
    const std::string kKey = "http://example.com/";

    http::Request get_request() {
        http::Request req;
        req.method = "GET";
        req.target = kKey;
        req.headers.add("Host", "example.com");
        return req;
    }

    http::Response ok_response(const std::string& cache_control, Clock::time_point date = kT0) {
        http::Response resp;
        resp.status = 200;
        resp.headers.add("Date", http::format_http_date(date));
        if (!cache_control.empty()) resp.headers.add("Cache-Control", cache_control);
        resp.body = "hello";
        return resp;
    }

}  // namespace

TEST_CASE("storability rules", "[http-cache]") {
    auto req = get_request();

    CHECK(cache::is_storable(req, ok_response("max-age=60")));
    CHECK_FALSE(cache::is_storable(req, ok_response("no-store")));
    CHECK_FALSE(cache::is_storable(req, ok_response("private, max-age=60")));
    // No freshness information and no validators: useless to store.
    CHECK_FALSE(cache::is_storable(req, ok_response("")));

    auto with_etag = ok_response("");
    with_etag.headers.add("ETag", "\"v1\"");
    CHECK(cache::is_storable(req, with_etag));

    auto post = req;
    post.method = "POST";
    CHECK_FALSE(cache::is_storable(post, ok_response("max-age=60")));

    auto no_store_req = req;
    no_store_req.headers.add("Cache-Control", "no-store");
    CHECK_FALSE(cache::is_storable(no_store_req, ok_response("max-age=60")));

    auto authed = req;
    authed.headers.add("Authorization", "Bearer x");
    CHECK_FALSE(cache::is_storable(authed, ok_response("max-age=60")));
    CHECK(cache::is_storable(authed, ok_response("public, max-age=60")));

    auto vary_star = ok_response("max-age=60");
    vary_star.headers.add("Vary", "*");
    CHECK_FALSE(cache::is_storable(req, vary_star));

    auto cookie = ok_response("max-age=60");
    cookie.headers.add("Set-Cookie", "session=1");
    CHECK_FALSE(cache::is_storable(req, cookie));

    auto partial = ok_response("max-age=60");
    partial.status = 206;
    CHECK_FALSE(cache::is_storable(req, partial));

    // 302 is not cacheable by default but explicit freshness allows it.
    auto redirect = ok_response("max-age=60");
    redirect.status = 302;
    CHECK(cache::is_storable(req, redirect));
}

TEST_CASE("freshness lifetime precedence", "[http-cache]") {
    auto resp = ok_response("max-age=60, s-maxage=120");
    CHECK(cache::freshness_lifetime(resp, kT0, 24h) == 120s);

    resp = ok_response("max-age=60");
    resp.headers.add("Expires", http::format_http_date(kT0 + 1000s));
    CHECK(cache::freshness_lifetime(resp, kT0, 24h) == 60s);

    resp = ok_response("");
    resp.headers.add("Expires", http::format_http_date(kT0 + 300s));
    CHECK(cache::freshness_lifetime(resp, kT0, 24h) == 300s);

    resp = ok_response("");
    resp.headers.add("Expires", "0");  // invalid date = already expired
    CHECK(cache::freshness_lifetime(resp, kT0, 24h) == 0s);

    // Heuristic: 10% of the time since Last-Modified, capped.
    resp = ok_response("");
    resp.headers.add("Last-Modified", http::format_http_date(kT0 - 1000s));
    CHECK(cache::freshness_lifetime(resp, kT0, 24h) == 100s);
    resp = ok_response("");
    resp.headers.add("Last-Modified", http::format_http_date(kT0 - 365 * 24h));
    CHECK(cache::freshness_lifetime(resp, kT0, 24h) == 24h);
}

TEST_CASE("fresh entries are served until they expire", "[http-cache]") {
    HttpCache cache;
    auto req = get_request();
    REQUIRE(cache.store(kKey, req, ok_response("max-age=60"), kT0, kT0));

    auto hit = cache.lookup(kKey, req, kT0 + 30s);
    CHECK(hit.status == LookupStatus::Fresh);
    REQUIRE(hit.entry);
    CHECK(hit.entry->response.body == "hello");
    CHECK(hit.entry->current_age(kT0 + 30s) == 30s);

    CHECK(cache.lookup(kKey, req, kT0 + 61s).status == LookupStatus::NeedsValidation);
    CHECK(cache.lookup("http://example.com/other", req, kT0).status == LookupStatus::Miss);

    auto stats = cache.stats();
    CHECK(stats.hits == 1);
    CHECK(stats.misses == 1);
    CHECK(stats.stores == 1);
}

TEST_CASE("Age header and response delay count towards the age", "[http-cache]") {
    HttpCache cache;
    auto req = get_request();
    auto resp = ok_response("max-age=60");
    resp.headers.add("Age", "50");
    // Request took 5 seconds: corrected age = 50 + 5.
    REQUIRE(cache.store(kKey, req, resp, kT0 - 5s, kT0));
    CHECK(cache.lookup(kKey, req, kT0 + 4s).status == LookupStatus::Fresh);
    CHECK(cache.lookup(kKey, req, kT0 + 6s).status == LookupStatus::NeedsValidation);
}

TEST_CASE("request directives change what counts as fresh", "[http-cache]") {
    HttpCache cache;
    auto req = get_request();
    REQUIRE(cache.store(kKey, req, ok_response("max-age=60"), kT0, kT0));

    auto no_cache = req;
    no_cache.headers.add("Cache-Control", "no-cache");
    CHECK(cache.lookup(kKey, no_cache, kT0).status == LookupStatus::NeedsValidation);

    auto pragma = req;
    pragma.headers.add("Pragma", "no-cache");
    CHECK(cache.lookup(kKey, pragma, kT0).status == LookupStatus::NeedsValidation);

    auto max_age = req;
    max_age.headers.add("Cache-Control", "max-age=10");
    CHECK(cache.lookup(kKey, max_age, kT0 + 5s).status == LookupStatus::Fresh);
    CHECK(cache.lookup(kKey, max_age, kT0 + 20s).status == LookupStatus::NeedsValidation);

    auto min_fresh = req;
    min_fresh.headers.add("Cache-Control", "min-fresh=30");
    CHECK(cache.lookup(kKey, min_fresh, kT0 + 20s).status == LookupStatus::Fresh);
    CHECK(cache.lookup(kKey, min_fresh, kT0 + 40s).status == LookupStatus::NeedsValidation);

    auto max_stale = req;
    max_stale.headers.add("Cache-Control", "max-stale=30");
    CHECK(cache.lookup(kKey, max_stale, kT0 + 80s).status == LookupStatus::Fresh);
    CHECK(cache.lookup(kKey, max_stale, kT0 + 100s).status == LookupStatus::NeedsValidation);
}

TEST_CASE("must-revalidate forbids serving stale responses", "[http-cache]") {
    HttpCache cache;
    auto req = get_request();
    REQUIRE(cache.store(kKey, req, ok_response("max-age=60, must-revalidate"), kT0, kT0));
    auto max_stale = req;
    max_stale.headers.add("Cache-Control", "max-stale");
    CHECK(cache.lookup(kKey, max_stale, kT0 + 120s).status == LookupStatus::NeedsValidation);
}

TEST_CASE("response no-cache requires revalidation on every use", "[http-cache]") {
    HttpCache cache;
    auto req = get_request();
    auto resp = ok_response("no-cache, max-age=600");
    resp.headers.add("ETag", "\"v1\"");
    REQUIRE(cache.store(kKey, req, resp, kT0, kT0));
    CHECK(cache.lookup(kKey, req, kT0).status == LookupStatus::NeedsValidation);
}

TEST_CASE("Vary selects on request headers", "[http-cache]") {
    HttpCache cache;
    auto req = get_request();
    req.headers.add("Accept-Encoding", "gzip");
    auto resp = ok_response("max-age=60");
    resp.headers.add("Vary", "Accept-Encoding");
    REQUIRE(cache.store(kKey, req, resp, kT0, kT0));

    CHECK(cache.lookup(kKey, req, kT0).status == LookupStatus::Fresh);
    auto other = get_request();
    other.headers.add("Accept-Encoding", "br");
    CHECK(cache.lookup(kKey, other, kT0).status == LookupStatus::Miss);
    CHECK(cache.lookup(kKey, get_request(), kT0).status == LookupStatus::Miss);
}

TEST_CASE("304 responses refresh the stored entry", "[http-cache]") {
    HttpCache cache;
    auto req = get_request();
    auto resp = ok_response("max-age=10");
    resp.headers.add("ETag", "\"v1\"");
    resp.headers.add("X-Version", "1");
    auto stored = cache.store(kKey, req, resp, kT0, kT0);
    REQUIRE(stored);

    auto later = kT0 + 100s;
    auto stale = cache.lookup(kKey, req, later);
    REQUIRE(stale.status == LookupStatus::NeedsValidation);

    http::Response not_modified;
    not_modified.status = 304;
    not_modified.headers.add("Date", http::format_http_date(later));
    not_modified.headers.add("Cache-Control", "max-age=30");
    not_modified.headers.add("X-Version", "2");
    not_modified.headers.add("Content-Length", "0");  // must not overwrite the stored length

    auto refreshed = cache.refresh(kKey, stale.entry, req, not_modified, later, later);
    REQUIRE(refreshed);
    CHECK(refreshed->response.status == 200);
    CHECK(refreshed->response.body == "hello");
    CHECK(refreshed->response.headers.get("X-Version") == "2");
    CHECK(refreshed->response.headers.get("ETag") == "\"v1\"");
    CHECK_FALSE(refreshed->response.headers.contains("Content-Length"));

    CHECK(cache.lookup(kKey, req, later + 20s).status == LookupStatus::Fresh);
    CHECK(cache.lookup(kKey, req, later + 40s).status == LookupStatus::NeedsValidation);
    CHECK(cache.stats().revalidations == 1);
}

TEST_CASE("size limits and LRU eviction", "[http-cache]") {
    cache::CacheOptions options;
    options.max_bytes = 2000;
    options.max_entry_bytes = 1000;
    HttpCache cache(options);
    auto req = get_request();

    auto big = ok_response("max-age=60");
    big.body.assign(1500, 'x');
    CHECK_FALSE(cache.store(kKey, req, big, kT0, kT0));

    auto medium = ok_response("max-age=60");
    medium.body.assign(600, 'x');
    REQUIRE(cache.store("http://a/1", req, medium, kT0, kT0));
    REQUIRE(cache.store("http://a/2", req, medium, kT0, kT0));
    REQUIRE(cache.store("http://a/3", req, medium, kT0, kT0));  // evicts /1

    CHECK(cache.lookup("http://a/1", req, kT0).status == LookupStatus::Miss);
    CHECK(cache.lookup("http://a/3", req, kT0).status == LookupStatus::Fresh);
    auto stats = cache.stats();
    CHECK(stats.evictions == 1);
    CHECK(stats.entries == 2);
    CHECK(stats.bytes <= options.max_bytes);
}

TEST_CASE("invalidate removes entries", "[http-cache]") {
    HttpCache cache;
    auto req = get_request();
    REQUIRE(cache.store(kKey, req, ok_response("max-age=60"), kT0, kT0));
    cache.invalidate(kKey);
    CHECK(cache.lookup(kKey, req, kT0).status == LookupStatus::Miss);
    CHECK(cache.stats().invalidations == 1);
}

TEST_CASE("conditional requests are detected", "[http-cache]") {
    auto req = get_request();
    CHECK_FALSE(cache::is_conditional(req));
    req.headers.add("If-None-Match", "\"v1\"");
    CHECK(cache::is_conditional(req));
}