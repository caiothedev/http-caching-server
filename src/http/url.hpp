#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace proxy::http {

    struct HostPort {
        std::string host;  // lowercase; IPv6 literals without brackets
        std::uint16_t port = 0;

        // "host:port", bracketing IPv6 literals.
        std::string to_string() const;
    };

    struct Url {
        std::string scheme;           // lowercase, e.g. "http"
        std::string host;             // lowercase; IPv6 literals without brackets
        std::uint16_t port = 0;       // explicit or scheme default
        std::string path_and_query;   // origin-form target, always starts with '/'

        std::uint16_t default_port() const;
        // Value for the Host header: port omitted when it is the default.
        std::string authority() const;
        // Normalized absolute URL (no fragment, default port omitted). Used as cache key.
        std::string to_string() const;
    };

    // Parses an absolute-form request target such as "http://example.com:8080/a?b".
    std::optional<Url> parse_absolute_url(std::string_view target);

    // Parses an authority-form target ("host:port", "[::1]:443"). If the port is
    // missing, `default_port` is used (pass 0 to require an explicit port).
    std::optional<HostPort> parse_authority(std::string_view authority, std::uint16_t default_port);

    // Resolves a Location / Content-Location value against the request URL.
    // Only same-origin absolute URLs and absolute paths are supported.
    std::optional<Url> resolve_reference(const Url& base, std::string_view reference);

}  // namespace proxy::http