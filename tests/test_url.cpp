#include <catch2/catch_test_macros.hpp>

#include "http/url.hpp"

using namespace proxy::http;

TEST_CASE("parses absolute URLs", "[url]") {
    auto url = parse_absolute_url("http://Example.COM/a/b?x=1#frag");
    REQUIRE(url);
    CHECK(url->scheme == "http");
    CHECK(url->host == "example.com");
    CHECK(url->port == 80);
    CHECK(url->path_and_query == "/a/b?x=1");
    CHECK(url->authority() == "example.com");
    CHECK(url->to_string() == "http://example.com/a/b?x=1");
}

TEST_CASE("handles explicit ports, empty paths and IPv6", "[url]") {
    auto a = parse_absolute_url("http://example.com:8080");
    REQUIRE(a);
    CHECK(a->port == 8080);
    CHECK(a->path_and_query == "/");
    CHECK(a->to_string() == "http://example.com:8080/");

    auto b = parse_absolute_url("http://example.com?q");
    REQUIRE(b);
    CHECK(b->path_and_query == "/?q");

    auto c = parse_absolute_url("http://[::1]:3000/x");
    REQUIRE(c);
    CHECK(c->host == "::1");
    CHECK(c->authority() == "[::1]:3000");

    // The default port is normalized away so both forms share a cache key.
    CHECK(parse_absolute_url("http://a:80/")->to_string() == parse_absolute_url("http://a/")->to_string());
}

TEST_CASE("rejects invalid absolute URLs", "[url]") {
    CHECK_FALSE(parse_absolute_url("/relative"));
    CHECK_FALSE(parse_absolute_url("ftp://example.com/"));
    CHECK_FALSE(parse_absolute_url("http:///path"));
    CHECK_FALSE(parse_absolute_url("http://host:99999/"));
    CHECK_FALSE(parse_absolute_url("http://ho st/"));
}

TEST_CASE("parses CONNECT authorities", "[url]") {
    auto a = parse_authority("example.com:443", 0);
    REQUIRE(a);
    CHECK(a->host == "example.com");
    CHECK(a->port == 443);

    auto b = parse_authority("[2001:db8::1]:8443", 0);
    REQUIRE(b);
    CHECK(b->host == "2001:db8::1");
    CHECK(b->port == 8443);
    CHECK(b->to_string() == "[2001:db8::1]:8443");

    CHECK_FALSE(parse_authority("example.com", 0));  // port required
    CHECK(parse_authority("example.com", 443)->port == 443);
    CHECK_FALSE(parse_authority(":443", 0));
    CHECK_FALSE(parse_authority("example.com:0", 0));
}

TEST_CASE("resolves same-origin references", "[url]") {
    auto base = *parse_absolute_url("http://example.com/items/1");
    CHECK(resolve_reference(base, "/items/2")->to_string() == "http://example.com/items/2");
    CHECK(resolve_reference(base, "http://example.com/x")->to_string() == "http://example.com/x");
    CHECK_FALSE(resolve_reference(base, "http://other.com/x"));
    CHECK_FALSE(resolve_reference(base, "//other.com/x"));
}