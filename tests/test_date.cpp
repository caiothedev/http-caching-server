#include <catch2/catch_test_macros.hpp>

#include "http/date.hpp"

using namespace proxy::http;

namespace {
    long long epoch_seconds(Clock::time_point tp) {
        return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    }
}  // namespace

TEST_CASE("parses the three HTTP date formats", "[date]") {
    constexpr long long kExpected = 784111777;  // Sun, 06 Nov 1994 08:49:37 GMT

    auto imf = parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT");
    REQUIRE(imf);
    CHECK(epoch_seconds(*imf) == kExpected);

    auto rfc850 = parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT");
    REQUIRE(rfc850);
    CHECK(epoch_seconds(*rfc850) == kExpected);

    auto asctime = parse_http_date("Sun Nov  6 08:49:37 1994");
    REQUIRE(asctime);
    CHECK(epoch_seconds(*asctime) == kExpected);
}

TEST_CASE("rejects invalid dates", "[date]") {
    CHECK_FALSE(parse_http_date(""));
    CHECK_FALSE(parse_http_date("0"));
    CHECK_FALSE(parse_http_date("-1"));
    CHECK_FALSE(parse_http_date("Sun, 06 Foo 1994 08:49:37 GMT"));
    CHECK_FALSE(parse_http_date("Sun, 32 Nov 1994 08:49:37 GMT"));
    CHECK_FALSE(parse_http_date("Sun, 06 Nov 1994 25:49:37 GMT"));
    // Day numbers that do not exist in the given month.
    CHECK_FALSE(parse_http_date("Thu, 31 Apr 2025 08:49:37 GMT"));
    CHECK_FALSE(parse_http_date("Sat, 29 Feb 2025 08:49:37 GMT"));
    CHECK(parse_http_date("Thu, 29 Feb 2024 08:49:37 GMT"));
}

TEST_CASE("formats IMF-fixdate and round-trips", "[date]") {
    Clock::time_point tp{std::chrono::seconds(784111777)};
    CHECK(format_http_date(tp) == "Sun, 06 Nov 1994 08:49:37 GMT");
    CHECK(format_http_date(Clock::time_point{}) == "Thu, 01 Jan 1970 00:00:00 GMT");

    Clock::time_point leap{std::chrono::seconds(951782400)};  // 2000-02-29
    CHECK(format_http_date(leap) == "Tue, 29 Feb 2000 00:00:00 GMT");
    CHECK(parse_http_date(format_http_date(leap)) == leap);
}