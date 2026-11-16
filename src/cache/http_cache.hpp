#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cache/lru_cache.hpp"
#include "http/date.hpp"
#include "http/message.hpp"

namespace proxy::cache {

    using Clock = http::Clock;
    using Seconds = std::chrono::seconds;

    struct CacheEntry {
        http::Response response;  // hop-by-hop headers already removed
        Clock::time_point request_time;
        Clock::time_point response_time;
        Seconds corrected_initial_age{0};  // RFC 9111 §4.2.3
        Seconds freshness_lifetime{0};     // RFC 9111 §4.2.1
        bool no_cache = false;             // response must be revalidated on every use
        bool must_revalidate = false;      // stale response may never be served
        // Request header values selected by the response's Vary field.
        std::vector<std::pair<std::string, std::string>> vary;
        std::size_t size_bytes = 0;

        Seconds current_age(Clock::time_point now) const;
        bool has_validators() const;
    };

    using EntryPtr = std::shared_ptr<const CacheEntry>;

    enum class LookupStatus {
        Miss,               // nothing usable stored
        Fresh,              // can be served without contacting the origin
        NeedsValidation,    // stored but stale (or no-cache): revalidate first
    };

    struct LookupResult {
        LookupStatus status = LookupStatus::Miss;
        EntryPtr entry;
    };

    struct CacheOptions {
        std::size_t max_bytes = 64 * 1024 * 1024;
        std::size_t max_entry_bytes = 8 * 1024 * 1024;
        Seconds max_heuristic_lifetime{24 * 3600};
    };

    struct CacheStats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t revalidations = 0;
        std::uint64_t stores = 0;
        std::uint64_t invalidations = 0;
        std::size_t evictions = 0;
        std::size_t entries = 0;
        std::size_t bytes = 0;
        std::size_t capacity = 0;
    };

    // Policy helpers (pure functions, exposed for unit tests)

    // Whether a shared cache may store this response (RFC 9111 §3).
    bool is_storable(const http::Request& req, const http::Response& resp);

    // Freshness lifetime: s-maxage > max-age > Expires - Date > heuristic (10% of
    // Date - Last-Modified, capped). Returns 0 when the response has no freshness info.
    Seconds freshness_lifetime(const http::Response& resp, Clock::time_point response_time,
                            Seconds max_heuristic);

    // Request carries its own validators/conditions; the cache then just forwards it.
    bool is_conditional(const http::Request& req);

    // Thread-safe shared HTTP cache keyed by normalized absolute URL.
    class HttpCache {
    public:
        explicit HttpCache(CacheOptions options = {});

        LookupResult lookup(const std::string& key, const http::Request& req, Clock::time_point now);

        // Stores the response if policy allows. Returns the stored entry or nullptr.
        EntryPtr store(const std::string& key, const http::Request& req, const http::Response& resp,
                    Clock::time_point request_time, Clock::time_point response_time);

        // Merges a 304 Not Modified into the stored entry (RFC 9111 §4.3.4) and
        // returns the updated entry, which should be sent to the client.
        EntryPtr refresh(const std::string& key, const EntryPtr& stale, const http::Request& req,
                        const http::Response& not_modified, Clock::time_point request_time,
                        Clock::time_point response_time);

        void invalidate(const std::string& key);
        CacheStats stats() const;
        const CacheOptions& options() const { return options_; }

    private:
        std::shared_ptr<CacheEntry> make_entry(const http::Request& req, http::Response resp,
                                            Clock::time_point request_time,
                                            Clock::time_point response_time) const;

        CacheOptions options_;
        mutable std::mutex mutex_;
        LruCache<std::string, EntryPtr> lru_;
        CacheStats counters_;
    };

}  // namespace proxy::cache