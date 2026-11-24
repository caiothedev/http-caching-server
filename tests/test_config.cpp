#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "proxy/config.hpp"

namespace {
    proxy::ParseOutcome parse(std::vector<std::string> args) {
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& arg : args) argv.push_back(arg.data());
        return proxy::parse_command_line(static_cast<int>(argv.size()), argv.data());
    }
}

TEST_CASE("command line defaults are applied", "[config]") {
    auto result = parse({"http_proxy"});
    REQUIRE(result.config);
    CHECK(result.exit_code == 0);
    CHECK(result.config->listen_address == "127.0.0.1");
    CHECK(result.config->port == 8080);
    CHECK(result.config->threads == 0);
    CHECK(result.config->max_connections == 16);
    CHECK(result.config->limits.max_body_bytes == 8u * 1024u * 1024u);
    CHECK(result.config->cache_enabled);
    CHECK((result.config->connect_ports == std::vector<std::uint16_t>{443}));
    CHECK(result.config->upstream_timeout == std::chrono::seconds{30});
}

TEST_CASE("command line accepts separated and inline values", "[config]") {
    auto result = parse({
        "http_proxy", "--listen", "0.0.0.0", "--port=9000", "--threads", "4",
        "--cache-size=32", "--max-object", "16", "--max-connections=4", "--max-body", "16",
        "--timeout=45", "--idle-timeout", "90",
        "--tunnel-timeout=600", "--connect-ports", "443,8443", "--log-level", "debug",
    });
    REQUIRE(result.config);
    CHECK(result.config->listen_address == "0.0.0.0");
    CHECK(result.config->port == 9000);
    CHECK(result.config->threads == 4);
    CHECK(result.config->max_connections == 4);
    CHECK(result.config->limits.max_body_bytes == 16u * 1024u * 1024u);
    CHECK(result.config->cache.max_bytes == 32u * 1024u * 1024u);
    CHECK(result.config->cache.max_entry_bytes == 16u * 1024u * 1024u);
    CHECK(result.config->upstream_timeout == std::chrono::seconds{45});
    CHECK(result.config->client_idle_timeout == std::chrono::seconds{90});
    CHECK(result.config->tunnel_idle_timeout == std::chrono::seconds{600});
    CHECK((result.config->connect_ports == std::vector<std::uint16_t>{443, 8443}));
    CHECK(result.config->log_level == proxy::log::Level::Debug);
}

TEST_CASE("numeric options enforce their documented ranges", "[config]") {
    const std::vector<std::vector<std::string>> invalid = {
        {"http_proxy", "--port", "0"}, {"http_proxy", "--port", "65536"},
        {"http_proxy", "--port", "abc"}, {"http_proxy", "--threads", "0"},
        {"http_proxy", "--threads", "1025"}, {"http_proxy", "--max-connections", "0"},
        {"http_proxy", "--max-connections", "1025"}, {"http_proxy", "--max-body", "0"},
        {"http_proxy", "--max-body", "129"}, {"http_proxy", "--cache-size", "0"},
        {"http_proxy", "--cache-size", "1048577"}, {"http_proxy", "--max-object", "0"},
        {"http_proxy", "--timeout", "3601"}, {"http_proxy", "--idle-timeout", "-1"},
        {"http_proxy", "--tunnel-timeout", "86401"}, {"http_proxy", "--connect-ports", ""},
        {"http_proxy", "--connect-ports", "443,0"}, {"http_proxy", "--connect-ports", "443,65536"},
    };
    for (const auto& args : invalid) {
        CAPTURE(args);
        auto result = parse(args);
        CHECK_FALSE(result.config);
        CHECK(result.exit_code == 2);
        CHECK_FALSE(result.message.empty());
    }
}

TEST_CASE("aggregate in-flight body budget is enforced", "[config]") {
    auto default_budget = parse({"http_proxy", "--max-connections", "16", "--max-body", "8"});
    REQUIRE(default_budget.config);
    auto higher_concurrency = parse({"http_proxy", "--max-connections", "32", "--max-body", "4"});
    REQUIRE(higher_concurrency.config);
    auto invalid = parse({"http_proxy", "--max-connections", "17", "--max-body", "8"});
    CHECK_FALSE(invalid.config);
    CHECK(invalid.exit_code == 2);
}

TEST_CASE("cache object size is clamped to cache capacity and body limit", "[config]") {
    auto result = parse({"http_proxy", "--cache-size", "4", "--max-object", "8", "--max-body", "2"});
    REQUIRE(result.config);
    CHECK(result.config->cache.max_bytes == 4u * 1024u * 1024u);
    CHECK(result.config->cache.max_entry_bytes == 2u * 1024u * 1024u);
    CHECK(result.config->limits.max_body_bytes == result.config->cache.max_entry_bytes);
}

TEST_CASE("CONNECT port policy accepts any or a validated list", "[config]") {
    auto any = parse({"http_proxy", "--connect-ports", "any"});
    REQUIRE(any.config);
    CHECK(any.config->connect_ports.empty());
    auto list = parse({"http_proxy", "--connect-ports=443,8443,443"});
    REQUIRE(list.config);
    CHECK((list.config->connect_ports == std::vector<std::uint16_t>{443, 8443, 443}));
}

TEST_CASE("missing values and unknown options are rejected", "[config]") {
    const std::vector<std::vector<std::string>> invalid = {
        {"http_proxy", "--listen"}, {"http_proxy", "--port"}, {"http_proxy", "--threads"},
        {"http_proxy", "--cache-size"}, {"http_proxy", "--max-object"}, {"http_proxy", "--timeout"},
        {"http_proxy", "--idle-timeout"}, {"http_proxy", "--tunnel-timeout"},
        {"http_proxy", "--connect-ports"}, {"http_proxy", "--log-level"}, {"http_proxy", "--unknown"},
    };
    for (const auto& args : invalid) {
        CAPTURE(args);
        auto result = parse(args);
        CHECK_FALSE(result.config);
        CHECK(result.exit_code == 2);
    }
}

TEST_CASE("help exits successfully without producing a config", "[config]") {
    auto result = parse({"http_proxy", "--help"});
    CHECK_FALSE(result.config);
    CHECK(result.exit_code == 0);
    CHECK(result.message.find("Usage: http_proxy") != std::string::npos);
}

TEST_CASE("access policy options are parsed", "[config]") {
    auto defaults = parse({"http_proxy"});
    REQUIRE(defaults.config);
    CHECK(defaults.config->access.allowed_hosts.empty());
    CHECK_FALSE(defaults.config->access.block_private);

    auto result = parse({"http_proxy", "--allow-hosts", "Example.com, *.cdn.test", "--block-private"});
    REQUIRE(result.config);
    CHECK((result.config->access.allowed_hosts == std::vector<std::string>{"example.com", "*.cdn.test"}));
    CHECK(result.config->access.block_private);

    auto inline_value = parse({"http_proxy", "--allow-hosts=api.test"});
    REQUIRE(inline_value.config);
    CHECK((inline_value.config->access.allowed_hosts == std::vector<std::string>{"api.test"}));
}

TEST_CASE("invalid access policy options are rejected", "[config]") {
    for (auto value : {"", ",", "bad host", "*", "ex*ample.com"}) {
        INFO(value);
        auto result = parse({"http_proxy", "--allow-hosts", value});
        CHECK_FALSE(result.config);
        CHECK(result.exit_code == 2);
    }
    CHECK_FALSE(parse({"http_proxy", "--allow-hosts"}).config);
}

TEST_CASE("upstream pool options are parsed", "[config]") {
    auto defaults = parse({"http_proxy"});
    REQUIRE(defaults.config);
    CHECK(defaults.config->pool_size == 32);
    CHECK(defaults.config->pool_idle_timeout == std::chrono::seconds{15});

    auto result = parse({"http_proxy", "--pool-size", "0", "--pool-idle-timeout=5"});
    REQUIRE(result.config);
    CHECK(result.config->pool_size == 0);
    CHECK(result.config->pool_idle_timeout == std::chrono::seconds{5});

    CHECK_FALSE(parse({"http_proxy", "--pool-size", "1025"}).config);
    CHECK_FALSE(parse({"http_proxy", "--pool-idle-timeout", "0"}).config);
}