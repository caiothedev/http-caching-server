#include "proxy/access.hpp"

#include <array>
#include <cstdint>

#include "util/strings.hpp"

namespace proxy {

    namespace ip = boost::asio::ip;

    namespace {

        struct V4Range {
            std::uint32_t network;
            unsigned prefix;
        };

        constexpr std::uint32_t v4(unsigned a, unsigned b, unsigned c, unsigned d) {
            return (a << 24) | (b << 16) | (c << 8) | d;
        }

        // IANA special-purpose IPv4 ranges that are not globally reachable.
        constexpr std::array<V4Range, 14> kNonPublicV4 = {{
            {v4(0, 0, 0, 0), 8},        // "this network"
            {v4(10, 0, 0, 0), 8},       // private
            {v4(100, 64, 0, 0), 10},    // carrier-grade NAT
            {v4(127, 0, 0, 0), 8},      // loopback
            {v4(169, 254, 0, 0), 16},   // link-local (includes cloud metadata 169.254.169.254)
            {v4(172, 16, 0, 0), 12},    // private
            {v4(192, 0, 0, 0), 24},     // IETF protocol assignments
            {v4(192, 0, 2, 0), 24},     // documentation (TEST-NET-1)
            {v4(192, 168, 0, 0), 16},   // private
            {v4(198, 18, 0, 0), 15},    // benchmarking
            {v4(198, 51, 100, 0), 24},  // documentation (TEST-NET-2)
            {v4(203, 0, 113, 0), 24},   // documentation (TEST-NET-3)
            {v4(224, 0, 0, 0), 4},      // multicast
            {v4(240, 0, 0, 0), 4},      // reserved, including broadcast
        }};

        bool non_public_v4(std::uint32_t addr) {
            for (const auto& range : kNonPublicV4) {
                std::uint32_t mask = range.prefix == 0 ? 0 : ~std::uint32_t{0} << (32 - range.prefix);
                if ((addr & mask) == range.network) return true;
            }
            return false;
        }

        std::uint32_t v4_at(const ip::address_v6::bytes_type& b, std::size_t offset) {
            return (std::uint32_t{b[offset]} << 24) | (std::uint32_t{b[offset + 1]} << 16) |
                   (std::uint32_t{b[offset + 2]} << 8) | std::uint32_t{b[offset + 3]};
        }

        bool non_public_v6(const ip::address_v6& addr) {
            const auto b = addr.to_bytes();
            bool first_80_zero = true;
            for (std::size_t i = 0; i < 10; ++i) first_80_zero = first_80_zero && b[i] == 0;

            // ::ffff:a.b.c.d (v4-mapped): judge the embedded IPv4 address.
            if (first_80_zero && b[10] == 0xff && b[11] == 0xff) return non_public_v4(v4_at(b, 12));
            // ::/96 covers :: (unspecified), ::1 (loopback) and deprecated v4-compatible addresses.
            if (first_80_zero && b[10] == 0 && b[11] == 0) return true;

            // 64:ff9b::/96 (NAT64): embedded IPv4.
            if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) {
                bool zero = true;
                for (std::size_t i = 4; i < 12; ++i) zero = zero && b[i] == 0;
                if (zero) return non_public_v4(v4_at(b, 12));
                if (b[4] == 0x00 && b[5] == 0x01) return true;  // 64:ff9b:1::/48 local-use NAT64
            }
            // 2002::/16 (6to4): embedded IPv4 in bits 16..47.
            if (b[0] == 0x20 && b[1] == 0x02) return non_public_v4(v4_at(b, 2));

            if ((b[0] & 0xfe) == 0xfc) return true;                  // fc00::/7 unique local
            if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;  // fe80::/10 link-local
            if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return true;  // fec0::/10 site-local (deprecated)
            if (b[0] == 0xff) return true;                           // ff00::/8 multicast
            if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return true;  // 2001:db8::/32
            if (b[0] == 0x01 && b[1] == 0x00 && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0 &&
                b[6] == 0 && b[7] == 0) {
                return true;  // 100::/64 discard-only
            }
            return false;
        }

        bool valid_host_chars(std::string_view s) {
            if (s.empty()) return false;
            for (char c : s) {
                bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                          c == '_' || c == ':';
                if (!ok) return false;
            }
            return s.front() != '.' && s.back() != '.' && s.find("..") == std::string_view::npos;
        }

        std::string_view without_trailing_dot(std::string_view host) {
            if (!host.empty() && host.back() == '.') host.remove_suffix(1);
            return host;
        }

    }  // namespace

    bool is_non_public_address(const ip::address& address) {
        if (address.is_v4()) return non_public_v4(address.to_v4().to_uint());
        return non_public_v6(address.to_v6());
    }

    bool parse_host_pattern(std::string_view pattern, std::string& out) {
        auto lower = util::to_lower(util::trim(pattern));
        std::string_view p = lower;
        if (p.size() >= 2 && p.front() == '[' && p.back() == ']') p = p.substr(1, p.size() - 2);
        p = without_trailing_dot(p);

        bool wildcard = p.substr(0, 2) == "*.";
        auto rest = wildcard ? p.substr(2) : p;
        if (!valid_host_chars(rest)) return false;
        out = std::string(p);
        return true;
    }

    bool AccessPolicy::host_allowed(std::string_view host) const {
        if (allowed_hosts.empty()) return true;
        auto h = util::to_lower(without_trailing_dot(host));
        for (const auto& pattern : allowed_hosts) {
            if (pattern.substr(0, 2) == "*.") {
                std::string_view suffix = std::string_view(pattern).substr(1);  // ".example.com"
                if (h.size() > suffix.size() &&
                    std::string_view(h).substr(h.size() - suffix.size()) == suffix) {
                    return true;
                }
            } else if (h == pattern) {
                return true;
            }
        }
        return false;
    }

    std::vector<ip::tcp::endpoint> AccessPolicy::filter(
        const ip::tcp::resolver::results_type& results) const {
        std::vector<ip::tcp::endpoint> out;
        for (const auto& entry : results) {
            if (address_allowed(entry.endpoint().address())) out.push_back(entry.endpoint());
        }
        return out;
    }

}  // namespace proxy