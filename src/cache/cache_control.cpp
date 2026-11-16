#include "cache/cache_control.hpp"

#include <limits>
#include <string>

#include "util/strings.hpp"

namespace proxy::cache {

    namespace {

        // Parses delta-seconds. Values too large saturate (RFC 9111 §1.2.2); garbage yields 0.
        std::int64_t parse_delta(std::string_view s) {
            std::uint64_t v = 0;
            if (util::parse_uint(s, v)) {
                constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
                return static_cast<std::int64_t>(v > kMax ? kMax : v);
            }
            // Digits followed by overflow: saturate; anything else: treat as 0 (stale).
            bool all_digits = !s.empty();
            for (char c : s) all_digits = all_digits && c >= '0' && c <= '9';
            return all_digits ? std::numeric_limits<std::int32_t>::max() : 0;
        }

    }  // namespace

    CacheControl parse_cache_control(std::string_view value) {
        CacheControl cc;

        // Split on commas that are not inside a quoted string.
        std::size_t start = 0;
        bool in_quotes = false;
        for (std::size_t i = 0; i <= value.size(); ++i) {
            if (i < value.size()) {
                char c = value[i];
                if (c == '"') in_quotes = !in_quotes;
                if (c != ',' || in_quotes) continue;
            }
            auto item = util::trim(value.substr(start, i - start));
            start = i + 1;
            if (item.empty()) continue;

            std::string_view name = item;
            std::string_view arg;
            bool has_arg = false;
            auto eq = item.find('=');
            if (eq != std::string_view::npos) {
                name = util::trim(item.substr(0, eq));
                arg = util::trim(item.substr(eq + 1));
                if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
                    arg = arg.substr(1, arg.size() - 2);
                }
                has_arg = true;
            }
            auto n = util::to_lower(name);

            if (n == "no-cache") cc.no_cache = true;
            else if (n == "no-store") cc.no_store = true;
            else if (n == "no-transform") cc.no_transform = true;
            else if (n == "must-revalidate") cc.must_revalidate = true;
            else if (n == "proxy-revalidate") cc.proxy_revalidate = true;
            else if (n == "public") cc.is_public = true;
            else if (n == "private") cc.is_private = true;
            else if (n == "only-if-cached") cc.only_if_cached = true;
            else if (n == "max-age") cc.max_age = parse_delta(arg);
            else if (n == "s-maxage") cc.s_maxage = parse_delta(arg);
            else if (n == "min-fresh") cc.min_fresh = parse_delta(arg);
            else if (n == "max-stale") {
                if (has_arg) cc.max_stale = parse_delta(arg);
                else cc.max_stale_any = true;
            }
        }
        return cc;
    }

}  // namespace proxy::cache