#include "cache/http_cache.hpp"

#include <algorithm>
#include <set>

#include "cache/cache_control.hpp"
#include "util/strings.hpp"

namespace proxy::cache {

    namespace {

        constexpr std::size_t kEntryOverhead = 256;

        // Status codes cacheable by default (RFC 9110 §15.1), i.e. that may use
        // heuristic freshness when no explicit expiration is given.
        bool heuristically_cacheable(int status) {
            switch (status) {
                case 200:
                case 203:
                case 204:
                case 300:
                case 301:
                case 308:
                case 404:
                case 405:
                case 410:
                case 414:
                case 501: return true;
                default: return false;
            }
        }

        Seconds non_negative(Clock::duration d) {
            auto s = std::chrono::duration_cast<Seconds>(d);
            return s.count() < 0 ? Seconds{0} : s;
        }

        std::size_t estimate_size(const http::Response& resp) {
            std::size_t size = kEntryOverhead + resp.body.size() + resp.reason.size();
            for (const auto& f : resp.headers) size += f.name.size() + f.value.size() + 4;
            return size;
        }

        CacheControl request_cache_control(const http::Request& req) {
            auto cc = parse_cache_control(req.headers.get_combined("Cache-Control"));
            // HTTP/1.0 compatibility: "Pragma: no-cache" when Cache-Control is absent.
            if (!req.headers.contains("Cache-Control") && req.headers.has_token("Pragma", "no-cache")) {
                cc.no_cache = true;
            }
            return cc;
        }

    }  // namespace

    // CacheEntry
    Seconds CacheEntry::current_age(Clock::time_point now) const {
        return corrected_initial_age + non_negative(now - response_time);
    }

    bool CacheEntry::has_validators() const {
        return response.headers.contains("ETag") || response.headers.contains("Last-Modified");
    }

    // Policy
    bool is_storable(const http::Request& req, const http::Response& resp) {
        if (req.method != "GET") return false;
        if (resp.status < 200 || resp.status == 206 || resp.status == 304) return false;

        auto req_cc = parse_cache_control(req.headers.get_combined("Cache-Control"));
        auto resp_cc = parse_cache_control(resp.headers.get_combined("Cache-Control"));

        if (req_cc.no_store || resp_cc.no_store) return false;
        if (resp_cc.is_private) return false;  // we are a shared cache

        // RFC 9111 §3.5: authenticated responses need explicit permission.
        if (req.headers.contains("Authorization") &&
            !(resp_cc.is_public || resp_cc.s_maxage || resp_cc.must_revalidate)) {
            return false;
        }

        auto vary = resp.headers.get_tokens("Vary");
        if (std::find(vary.begin(), vary.end(), "*") != vary.end()) return false;

        // Conservative choice: personalised responses are not shared unless marked public.
        if (resp.headers.contains("Set-Cookie") && !resp_cc.is_public) return false;

        bool explicit_freshness =
        resp_cc.max_age || resp_cc.s_maxage || resp.headers.contains("Expires") || resp_cc.is_public;
        if (explicit_freshness) return true;

        // Without explicit freshness we need a cacheable-by-default status and a
        // validator, otherwise the stored copy could never be reused.
        bool validators = resp.headers.contains("ETag") || resp.headers.contains("Last-Modified");
        return heuristically_cacheable(resp.status) && validators;
    }

    Seconds freshness_lifetime(const http::Response& resp, Clock::time_point response_time,
                            Seconds max_heuristic) {
        auto cc = parse_cache_control(resp.headers.get_combined("Cache-Control"));
        if (cc.s_maxage) return Seconds{*cc.s_maxage};
        if (cc.max_age) return Seconds{*cc.max_age};

        auto date_header = resp.headers.get("Date");
        auto date = date_header ? http::parse_http_date(*date_header) : std::nullopt;
        auto date_value = date.value_or(response_time);

        if (auto expires = resp.headers.get("Expires")) {
            auto exp = http::parse_http_date(*expires);
            if (!exp) return Seconds{0};  // invalid Expires means "already expired"
            return non_negative(*exp - date_value);
        }

        if (auto lm_header = resp.headers.get("Last-Modified");
            lm_header && heuristically_cacheable(resp.status)) {
            if (auto lm = http::parse_http_date(*lm_header); lm && *lm < date_value) {
                auto heuristic = non_negative(date_value - *lm) / 10;
                return std::min(heuristic, max_heuristic);
            }
        }
        return Seconds{0};
    }

    bool is_conditional(const http::Request& req) {
        return req.headers.contains("If-None-Match") || req.headers.contains("If-Modified-Since") ||
            req.headers.contains("If-Match") || req.headers.contains("If-Unmodified-Since") ||
            req.headers.contains("If-Range") || req.headers.contains("Range");
    }

    // HttpCache
    HttpCache::HttpCache(CacheOptions options) : options_(options), lru_(options.max_bytes) {}

    std::shared_ptr<CacheEntry> HttpCache::make_entry(const http::Request& req, http::Response resp,
                                                    Clock::time_point request_time,
                                                    Clock::time_point response_time) const {
        auto entry = std::make_shared<CacheEntry>();
        http::remove_hop_by_hop(resp.headers);

        // Age calculation, RFC 9111 §4.2.3.
        auto date_header = resp.headers.get("Date");
        auto date = date_header ? http::parse_http_date(*date_header) : std::nullopt;
        auto date_value = date.value_or(response_time);
        std::uint64_t age_value = 0;
        if (auto age = resp.headers.get("Age")) util::parse_uint(util::trim(*age), age_value);

        auto apparent_age = non_negative(response_time - date_value);
        auto response_delay = non_negative(response_time - request_time);
        auto corrected_age_value = Seconds{static_cast<std::int64_t>(age_value)} + response_delay;

        auto cc = parse_cache_control(resp.headers.get_combined("Cache-Control"));
        entry->request_time = request_time;
        entry->response_time = response_time;
        entry->corrected_initial_age = std::max(apparent_age, corrected_age_value);
        entry->freshness_lifetime = freshness_lifetime(resp, response_time, options_.max_heuristic_lifetime);
        entry->no_cache = cc.no_cache;
        // s-maxage implies proxy-revalidate for shared caches (RFC 9111 §5.2.2.10).
        entry->must_revalidate = cc.must_revalidate || cc.proxy_revalidate || cc.s_maxage.has_value();

        for (const auto& name : resp.headers.get_tokens("Vary")) {
            entry->vary.emplace_back(name, req.headers.get_combined(name));
        }
        entry->size_bytes = estimate_size(resp);
        entry->response = std::move(resp);
        return entry;
    }

    LookupResult HttpCache::lookup(const std::string& key, const http::Request& req, Clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = lru_.get(key);
        if (!found) {
            ++counters_.misses;
            return {};
        }
        const EntryPtr& entry = *found;

        for (const auto& [name, value] : entry->vary) {
            if (req.headers.get_combined(name) != value) {
                ++counters_.misses;
                return {};
            }
        }

        auto req_cc = request_cache_control(req);
        if (entry->no_cache || req_cc.no_cache) return {LookupStatus::NeedsValidation, entry};

        auto age = entry->current_age(now);
        auto lifetime = entry->freshness_lifetime;
        bool fresh = age < lifetime;

        if (fresh && req_cc.max_age && age.count() >= *req_cc.max_age) fresh = false;
        if (fresh && req_cc.min_fresh && (lifetime - age).count() < *req_cc.min_fresh) fresh = false;
        if (!fresh && !entry->must_revalidate) {
            auto staleness = (age - lifetime).count();
            if (req_cc.max_stale_any || (req_cc.max_stale && staleness <= *req_cc.max_stale)) fresh = true;
        }

        if (fresh) {
            ++counters_.hits;
            return {LookupStatus::Fresh, entry};
        }
        return {LookupStatus::NeedsValidation, entry};
    }

    EntryPtr HttpCache::store(const std::string& key, const http::Request& req, const http::Response& resp,
                            Clock::time_point request_time, Clock::time_point response_time) {
        if (!is_storable(req, resp)) return nullptr;
        if (resp.body.size() > options_.max_entry_bytes) return nullptr;

        auto entry = make_entry(req, resp, request_time, response_time);
        if (entry->size_bytes > options_.max_entry_bytes) return nullptr;

        std::lock_guard<std::mutex> lock(mutex_);
        if (!lru_.put(key, entry, entry->size_bytes)) return nullptr;
        ++counters_.stores;
        return entry;
    }

    EntryPtr HttpCache::refresh(const std::string& key, const EntryPtr& stale, const http::Request& req,
                                const http::Response& not_modified, Clock::time_point request_time,
                                Clock::time_point response_time) {
        http::Response merged = stale->response;

        // RFC 9111 §3.2: header fields in the 304 replace the stored ones, except
        // framing fields that describe the (absent) 304 body.
        static const std::set<std::string> kSkip = {
            "content-length", "transfer-encoding",
            "content-encoding",
            "content-range"
        };
        http::Headers update = not_modified.headers;
        http::remove_hop_by_hop(update);
        std::set<std::string> replaced;
        for (const auto& f : update) {
            auto name = util::to_lower(f.name);
            if (kSkip.count(name)) continue;
            if (replaced.insert(name).second) merged.headers.remove(name);
            merged.headers.add(f.name, f.value);
        }

        auto entry = make_entry(req, std::move(merged), request_time, response_time);

        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_.revalidations;
        bool keep = is_storable(req, entry->response) && entry->size_bytes <= options_.max_entry_bytes;
        if (!keep || !lru_.put(key, entry, entry->size_bytes)) lru_.erase(key);
        return entry;
    }

    void HttpCache::invalidate(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lru_.erase(key)) ++counters_.invalidations;
    }

    CacheStats HttpCache::stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        CacheStats s = counters_;
        s.evictions = lru_.evictions();
        s.entries = lru_.size();
        s.bytes = lru_.total_cost();
        s.capacity = lru_.capacity();
        return s;
    }

}  // namespace proxy::cache