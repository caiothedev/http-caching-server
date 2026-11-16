#include "http/url.hpp"

#include "util/strings.hpp"

namespace proxy::http {

    namespace {

        std::string bracket_if_ipv6(const std::string& host) {
            return host.find(':') != std::string::npos ? "[" + host + "]" : host;
        }

        std::uint16_t scheme_default_port(std::string_view scheme) {
            if (scheme == "http") return 80;
            if (scheme == "https") return 443;
            return 0;
        }

        bool valid_host_char(char c) {
            // Registered names: letters, digits, '-', '.', '_' (percent-encoding not supported).
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '.' || c == '_';
        }

    }  // namespace

    std::string HostPort::to_string() const { return bracket_if_ipv6(host) + ":" + std::to_string(port); }

    std::uint16_t Url::default_port() const { return scheme_default_port(scheme); }

    std::string Url::authority() const {
        auto h = bracket_if_ipv6(host);
        if (port == default_port()) return h;
        return h + ":" + std::to_string(port);
    }

    std::string Url::to_string() const { return scheme + "://" + authority() + path_and_query; }

    std::optional<HostPort> parse_authority(std::string_view authority, std::uint16_t default_port) {
        if (authority.empty()) return std::nullopt;

        // Drop deprecated userinfo ("user:pass@host").
        auto at = authority.rfind('@');
        if (at != std::string_view::npos) authority.remove_prefix(at + 1);

        HostPort out;
        std::string_view port_str;

        if (authority.front() == '[') {
            auto close = authority.find(']');
            if (close == std::string_view::npos || close == 1) return std::nullopt;
            out.host = util::to_lower(authority.substr(1, close - 1));
            for (char c : out.host) {
                bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                if (!hex && c != ':' && c != '.') return std::nullopt;
            }
            auto rest = authority.substr(close + 1);
            if (!rest.empty()) {
                if (rest.front() != ':') return std::nullopt;
                port_str = rest.substr(1);
            }
        } else {
            auto colon = authority.rfind(':');
            auto host = authority.substr(0, colon);
            if (colon != std::string_view::npos) port_str = authority.substr(colon + 1);
            if (host.empty()) return std::nullopt;
            for (char c : host) {
                if (!valid_host_char(c)) return std::nullopt;
            }
            out.host = util::to_lower(host);
        }

        if (port_str.empty()) {
            if (default_port == 0) return std::nullopt;
            out.port = default_port;
        } else {
            std::uint64_t port = 0;
            if (!util::parse_uint(port_str, port) || port == 0 || port > 65535) return std::nullopt;
            out.port = static_cast<std::uint16_t>(port);
        }
        return out;
    }

    std::optional<Url> parse_absolute_url(std::string_view target) {
        auto sep = target.find("://");
        if (sep == std::string_view::npos || sep == 0) return std::nullopt;

        Url url;
        url.scheme = util::to_lower(target.substr(0, sep));
        for (char c : url.scheme) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
            if (!ok) return std::nullopt;
        }
        auto default_port = scheme_default_port(url.scheme);
        if (default_port == 0) return std::nullopt;

        auto rest = target.substr(sep + 3);
        auto path_start = rest.find_first_of("/?#");
        auto authority = rest.substr(0, path_start);
        auto hp = parse_authority(authority, default_port);
        if (!hp) return std::nullopt;
        url.host = std::move(hp->host);
        url.port = hp->port;

        std::string_view path = path_start == std::string_view::npos ? std::string_view{} : rest.substr(path_start);
        auto hash = path.find('#');
        if (hash != std::string_view::npos) path = path.substr(0, hash);
        if (path.empty() || path.front() != '/') {
            url.path_and_query = "/";
            url.path_and_query += path;  // "?query" without a path becomes "/?query"
        } else {
            url.path_and_query = std::string(path);
        }
        for (char c : url.path_and_query) {
            if (c <= ' ' || c == 0x7f) return std::nullopt;
        }
        return url;
    }

    std::optional<Url> resolve_reference(const Url& base, std::string_view reference) {
        reference = util::trim(reference);
        if (reference.empty()) return std::nullopt;
        if (reference.front() == '/' && (reference.size() == 1 || reference[1] != '/')) {
            Url out = base;
            auto hash = reference.find('#');
            out.path_and_query = std::string(reference.substr(0, hash));
            return out;
        }
        auto url = parse_absolute_url(reference);
        if (!url || url->scheme != base.scheme || url->host != base.host || url->port != base.port) {
            return std::nullopt;
        }
        return url;
    }

}  // namespace proxy::http