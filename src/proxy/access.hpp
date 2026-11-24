#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace proxy {

    // True for addresses a proxy reachable by untrusted clients should not
    // connect to: loopback, private (RFC 1918 / ULA), link-local, CGNAT,
    // unspecified, multicast, documentation and other reserved ranges. IPv4
    // addresses embedded in IPv6 (v4-mapped, NAT64, 6to4) are checked as IPv4.
    bool is_non_public_address(const boost::asio::ip::address& address);

    // Normalizes a host pattern for AccessPolicy::allowed_hosts: lowercase,
    // without a trailing dot or IPv6 brackets. Accepts "example.com" (exact
    // match) and "*.example.com" (any subdomain, not the apex itself).
    // Returns false for malformed patterns.
    bool parse_host_pattern(std::string_view pattern, std::string& out);

    // Which destinations the proxy may contact, for both forwarded requests and
    // CONNECT tunnels.
    struct AccessPolicy {
        std::vector<std::string> allowed_hosts;  // normalized patterns; empty = any host
        bool block_private = false;              // refuse non-public resolved addresses

        // `host` as found in the request (lowercase, IPv6 without brackets).
        bool host_allowed(std::string_view host) const;
        bool address_allowed(const boost::asio::ip::address& address) const {
            return !block_private || !is_non_public_address(address);
        }
        // Resolved endpoints that may be connected to. Filtering after DNS
        // resolution (instead of on the host name) also covers names that
        // resolve to internal addresses.
        std::vector<boost::asio::ip::tcp::endpoint> filter(
            const boost::asio::ip::tcp::resolver::results_type& results) const;
    };

}  // namespace proxyF