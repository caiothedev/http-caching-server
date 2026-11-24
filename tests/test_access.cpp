#include <string>

#include <catch2/catch_test_macros.hpp>

#include "proxy/access.hpp"

namespace {
    bool non_public(const char* text) {
        return proxy::is_non_public_address(boost::asio::ip::make_address(text));
    }

    proxy::AccessPolicy allow(std::initializer_list<const char*> patterns) {
        proxy::AccessPolicy policy;
        for (auto p : patterns) {
            std::string normalized;
            REQUIRE(proxy::parse_host_pattern(p, normalized));
            policy.allowed_hosts.push_back(normalized);
        }
        return policy;
    }
}  // namespace

TEST_CASE("non-public IPv4 ranges are recognized", "[access]") {
    for (auto addr : {"0.0.0.0", "10.1.2.3", "100.64.0.1", "127.0.0.1", "127.255.255.255", "169.254.169.254",
                      "172.16.0.1", "172.31.255.255", "192.168.1.1", "192.0.2.10", "198.18.0.1", "224.0.0.1",
                      "255.255.255.255"}) {
        INFO(addr);
        CHECK(non_public(addr));
    }
    for (auto addr : {"8.8.8.8", "1.1.1.1", "172.15.255.255", "172.32.0.1", "100.128.0.1", "93.184.216.34"}) {
        INFO(addr);
        CHECK_FALSE(non_public(addr));
    }
}

TEST_CASE("non-public IPv6 ranges are recognized, including embedded IPv4", "[access]") {
    for (auto addr : {"::", "::1", "fc00::1", "fd12:3456::1", "fe80::1", "ff02::1", "2001:db8::1",
                      "::ffff:127.0.0.1", "::ffff:10.0.0.1", "64:ff9b::a00:1", "2002:7f00:1::", "100::1"}) {
        INFO(addr);
        CHECK(non_public(addr));
    }
    for (auto addr : {"2606:4700:4700::1111", "2001:4860:4860::8888", "::ffff:8.8.8.8", "64:ff9b::808:808"}) {
        INFO(addr);
        CHECK_FALSE(non_public(addr));
    }
}

TEST_CASE("host patterns are normalized and validated", "[access]") {
    std::string out;
    CHECK(proxy::parse_host_pattern("Example.COM.", out));
    CHECK(out == "example.com");
    CHECK(proxy::parse_host_pattern("*.example.com", out));
    CHECK(out == "*.example.com");
    CHECK(proxy::parse_host_pattern("[::1]", out));
    CHECK(out == "::1");
    CHECK_FALSE(proxy::parse_host_pattern("", out));
    CHECK_FALSE(proxy::parse_host_pattern("*", out));
    CHECK_FALSE(proxy::parse_host_pattern("*.", out));
    CHECK_FALSE(proxy::parse_host_pattern("exa mple.com", out));
    CHECK_FALSE(proxy::parse_host_pattern("example..com", out));
    CHECK_FALSE(proxy::parse_host_pattern("ex*ample.com", out));
}

TEST_CASE("an empty allowlist permits every host", "[access]") {
    proxy::AccessPolicy policy;
    CHECK(policy.host_allowed("anything.test"));
}

TEST_CASE("allowlist matches exact hosts and wildcard subdomains", "[access]") {
    auto policy = allow({"example.com", "*.cdn.test"});
    CHECK(policy.host_allowed("example.com"));
    CHECK(policy.host_allowed("example.com."));
    CHECK(policy.host_allowed("EXAMPLE.com"));
    CHECK_FALSE(policy.host_allowed("www.example.com"));
    CHECK_FALSE(policy.host_allowed("badexample.com"));

    CHECK(policy.host_allowed("a.cdn.test"));
    CHECK(policy.host_allowed("a.b.cdn.test"));
    CHECK_FALSE(policy.host_allowed("cdn.test"));
    CHECK_FALSE(policy.host_allowed("evilcdn.test"));
}

TEST_CASE("address filtering only applies when private destinations are blocked", "[access]") {
    proxy::AccessPolicy policy;
    auto loopback = boost::asio::ip::make_address("127.0.0.1");
    CHECK(policy.address_allowed(loopback));
    policy.block_private = true;
    CHECK_FALSE(policy.address_allowed(loopback));
    CHECK(policy.address_allowed(boost::asio::ip::make_address("8.8.8.8")));
}