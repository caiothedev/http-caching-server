#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace proxy::http {

    using Clock = std::chrono::system_clock;

    // Parses an HTTP-date (RFC 9110 §5.6.7): IMF-fixdate, plus the obsolete
    // RFC 850 and asctime formats that recipients must still accept.
    std::optional<Clock::time_point> parse_http_date(std::string_view value);

    // Formats as IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
    std::string format_http_date(Clock::time_point tp);

}  // namespace proxy::http