#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace proxy::cache {

    // Parsed Cache-Control directives (RFC 9111 §5.2). Request and response
    // directives share one struct; each side only looks at what applies to it.
    struct CacheControl {
        // Shared
        bool no_cache = false;
        bool no_store = false;
        bool no_transform = false;
        std::optional<std::int64_t> max_age;

        // Response only
        bool must_revalidate = false;
        bool proxy_revalidate = false;
        bool is_public = false;
        bool is_private = false;
        std::optional<std::int64_t> s_maxage;

        // Request only
        bool only_if_cached = false;
        std::optional<std::int64_t> min_fresh;
        bool max_stale_any = false;  // "max-stale" without a value
        std::optional<std::int64_t> max_stale;
    };

    // Unknown directives are ignored. Qualified forms such as no-cache="Set-Cookie"
    // are treated as their unqualified (stricter) form. Invalid numeric values for
    // max-age make the response stale, as RFC 9111 §4.2.1 recommends.
    CacheControl parse_cache_control(std::string_view value);

}  // namespace proxy::cache