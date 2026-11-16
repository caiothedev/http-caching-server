#include <catch2/catch_test_macros.hpp>

#include <string>

#include "cache/lru_cache.hpp"

using proxy::cache::LruCache;

TEST_CASE("LRU stores and retrieves values", "[lru]") {
    LruCache<std::string, int> lru(100);
    REQUIRE(lru.put("a", 1, 10));
    REQUIRE(lru.put("b", 2, 10));
    CHECK(lru.get("a") == 1);
    CHECK(lru.get("b") == 2);
    CHECK_FALSE(lru.get("c").has_value());
    CHECK(lru.size() == 2);
    CHECK(lru.total_cost() == 20);
}

TEST_CASE("LRU evicts least recently used entries by cost", "[lru]") {
    LruCache<std::string, int> lru(30);
    lru.put("a", 1, 10);
    lru.put("b", 2, 10);
    lru.put("c", 3, 10);

    // Touch "a" so "b" becomes the least recently used.
    REQUIRE(lru.get("a") == 1);
    lru.put("d", 4, 10);

    CHECK(lru.contains("a"));
    CHECK_FALSE(lru.contains("b"));
    CHECK(lru.contains("c"));
    CHECK(lru.contains("d"));
    CHECK(lru.evictions() == 1);
    CHECK(lru.total_cost() == 30);
}

TEST_CASE("LRU peek does not change recency", "[lru]") {
    LruCache<std::string, int> lru(20);
    lru.put("a", 1, 10);
    lru.put("b", 2, 10);
    CHECK(lru.peek("a") == 1);
    lru.put("c", 3, 10);
    CHECK_FALSE(lru.contains("a"));
}

TEST_CASE("LRU replaces existing keys and updates cost", "[lru]") {
    LruCache<std::string, int> lru(100);
    lru.put("a", 1, 10);
    lru.put("a", 2, 50);
    CHECK(lru.size() == 1);
    CHECK(lru.total_cost() == 50);
    CHECK(lru.get("a") == 2);
}

TEST_CASE("LRU rejects items larger than capacity", "[lru]") {
    LruCache<std::string, int> lru(10);
    lru.put("a", 1, 5);
    CHECK_FALSE(lru.put("big", 2, 11));
    CHECK_FALSE(lru.contains("big"));
    CHECK(lru.contains("a"));

    // Replacing an existing key with an oversized value removes it.
    CHECK_FALSE(lru.put("a", 3, 11));
    CHECK_FALSE(lru.contains("a"));
    CHECK(lru.total_cost() == 0);
}

TEST_CASE("LRU erase and clear", "[lru]") {
    LruCache<std::string, int> lru(100);
    lru.put("a", 1, 10);
    lru.put("b", 2, 10);
    CHECK(lru.erase("a"));
    CHECK_FALSE(lru.erase("a"));
    CHECK(lru.total_cost() == 10);
    lru.clear();
    CHECK(lru.size() == 0);
    CHECK(lru.total_cost() == 0);
}