#include "http/date.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

#include "util/strings.hpp"

namespace proxy::http {

    namespace {

        constexpr std::array<const char*, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        constexpr std::array<const char*, 7> kDays = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

        // Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
        std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
            y -= m <= 2;
            const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
            const auto yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
        }

        void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
            z += 719468;
            const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            const auto doe = static_cast<unsigned>(z - era * 146097);
            const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const unsigned mp = (5 * doy + 2) / 153;
            d = doy - (153 * mp + 2) / 5 + 1;
            m = mp < 10 ? mp + 3 : mp - 9;
            y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2);
        }

        int month_index(std::string_view s) {
            for (std::size_t i = 0; i < kMonths.size(); ++i) {
                if (s == kMonths[i]) return static_cast<int>(i) + 1;
            }
            return 0;
        }

        bool digits(std::string_view s, int& out) {
            std::uint64_t v = 0;
            if (!util::parse_uint(s, v) || v > 100000) return false;
            out = static_cast<int>(v);
            return true;
        }

        bool parse_time(std::string_view s, int& h, int& mi, int& se) {
            if (s.size() != 8 || s[2] != ':' || s[5] != ':') return false;
            return digits(s.substr(0, 2), h) && digits(s.substr(3, 2), mi) && digits(s.substr(6, 2), se);
        }

        int days_in_month(int year, int month) {
            static constexpr int kMonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
            return month == 2 && leap ? 29 : kMonthDays[month - 1];
        }

        std::optional<Clock::time_point> build(int year, int month, int day, int h, int mi, int se) {
            if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month) || h > 23 || mi > 59 ||
                se > 60) {
                return std::nullopt;
            }
            auto days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
            auto secs = days * 86400 + h * 3600 + mi * 60 + se;
            return Clock::time_point(std::chrono::seconds(secs));
        }

    }  // namespace

    std::optional<Clock::time_point> parse_http_date(std::string_view v) {
        v = util::trim(v);
        int day = 0, year = 0, h = 0, mi = 0, se = 0;

        // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
        if (v.size() == 29 && v[3] == ',' && v[4] == ' ' && v.substr(25) == " GMT") {
            int month = month_index(v.substr(8, 3));
            if (v[7] != ' ' || v[11] != ' ' || v[16] != ' ' || month == 0) return std::nullopt;
            if (!digits(v.substr(5, 2), day) || !digits(v.substr(12, 4), year) ||
                !parse_time(v.substr(17, 8), h, mi, se)) {
                return std::nullopt;
            }
            return build(year, month, day, h, mi, se);
        }

        // RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT"
        auto comma = v.find(", ");
        if (comma != std::string_view::npos && v.size() == comma + 24 && v.substr(v.size() - 4) == " GMT") {
            auto rest = v.substr(comma + 2);  // "06-Nov-94 08:49:37 GMT"
            int month = month_index(rest.substr(3, 3));
            if (rest[2] != '-' || rest[6] != '-' || rest[9] != ' ' || month == 0) return std::nullopt;
            if (!digits(rest.substr(0, 2), day) || !digits(rest.substr(7, 2), year) ||
                !parse_time(rest.substr(10, 8), h, mi, se)) {
                return std::nullopt;
            }
            // RFC 9110: two-digit years more than 50 years in the future are in the past century.
            year += (year < 70) ? 2000 : 1900;
            return build(year, month, day, h, mi, se);
        }

        // asctime: "Sun Nov  6 08:49:37 1994"
        if (v.size() == 24 && v[3] == ' ' && v[7] == ' ' && v[10] == ' ' && v[19] == ' ') {
            int month = month_index(v.substr(4, 3));
            auto day_str = v.substr(8, 2);
            if (day_str[0] == ' ') day_str.remove_prefix(1);
            if (month == 0 || !digits(day_str, day) || !parse_time(v.substr(11, 8), h, mi, se) ||
                !digits(v.substr(20, 4), year)) {
                return std::nullopt;
            }
            return build(year, month, day, h, mi, se);
        }

        return std::nullopt;
    }

    std::string format_http_date(Clock::time_point tp) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
        std::int64_t days = secs / 86400;
        std::int64_t rem = secs % 86400;
        if (rem < 0) {
            rem += 86400;
            --days;
        }
        std::int64_t y = 0;
        unsigned m = 0, d = 0;
        civil_from_days(days, y, m, d);
        auto weekday = static_cast<std::size_t>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s, %02u %s %04lld %02lld:%02lld:%02lld GMT", kDays[weekday], d,
                    kMonths[m - 1], static_cast<long long>(y), static_cast<long long>(rem / 3600),
                    static_cast<long long>((rem / 60) % 60), static_cast<long long>(rem % 60));
        return buf;
    }

}  // namespace proxy::http