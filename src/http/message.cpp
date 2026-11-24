#include "http/message.hpp"

#include <algorithm>

#include "util/strings.hpp"

namespace proxy::http {

    using util::iequals;

    void Headers::add(std::string name, std::string value) {
        fields_.push_back({std::move(name), std::move(value)});
    }

    void Headers::set(std::string name, std::string value) {
        remove(name);
        add(std::move(name), std::move(value));
    }

    std::size_t Headers::remove(std::string_view name) {
        auto old_size = fields_.size();
        fields_.erase(std::remove_if(fields_.begin(), fields_.end(),
                                    [&](const HeaderField& f) { return iequals(f.name, name); }),
                    fields_.end());
        return old_size - fields_.size();
    }

    bool Headers::contains(std::string_view name) const {
        return std::any_of(fields_.begin(), fields_.end(),
                        [&](const HeaderField& f) { return iequals(f.name, name); });
    }

    std::optional<std::string> Headers::get(std::string_view name) const {
        for (const auto& f : fields_) {
            if (iequals(f.name, name)) return f.value;
        }
        return std::nullopt;
    }

    std::string Headers::get_combined(std::string_view name) const {
        std::string out;
        for (const auto& f : fields_) {
            if (!iequals(f.name, name)) continue;
            if (!out.empty()) out += ", ";
            out += f.value;
        }
        return out;
    }

    std::vector<std::string> Headers::get_tokens(std::string_view name) const {
        std::vector<std::string> out;
        for (const auto& f : fields_) {
            if (!iequals(f.name, name)) continue;
            for (auto token : util::split_list(f.value)) out.push_back(util::to_lower(token));
        }
        return out;
    }

    bool Headers::has_token(std::string_view name, std::string_view token) const {
        auto tokens = get_tokens(name);
        return std::any_of(tokens.begin(), tokens.end(),
                        [&](const std::string& t) { return iequals(t, token); });
    }

    std::string to_string(Version v) {
        return "HTTP/" + std::to_string(v.major) + "." + std::to_string(v.minor);
    }

    namespace {
        void append_headers(std::string& out, const Headers& headers) {
            for (const auto& f : headers) {
                out += f.name;
                out += ": ";
                out += f.value;
                out += "\r\n";
            }
            out += "\r\n";
        }
    }  // namespace

    std::string serialize(const Request& req) {
        std::string out;
        out.reserve(256 + req.body.size());
        out += req.method;
        out += ' ';
        out += req.target;
        out += ' ';
        out += to_string(req.version);
        out += "\r\n";
        append_headers(out, req.headers);
        out += req.body;
        return out;
    }

    std::string serialize_head(const Response& resp) {
        std::string out;
        out.reserve(256);
        out += to_string(resp.version);
        out += ' ';
        out += std::to_string(resp.status);
        out += ' ';
        out += resp.reason.empty() ? std::string(reason_phrase(resp.status)) : resp.reason;
        out += "\r\n";
        append_headers(out, resp.headers);
        return out;
    }

    std::string serialize(const Response& resp) {
        std::string out;
        out.reserve(256 + resp.body.size());
        out += to_string(resp.version);
        out += ' ';
        out += std::to_string(resp.status);
        out += ' ';
        out += resp.reason.empty() ? std::string(reason_phrase(resp.status)) : resp.reason;
        out += "\r\n";
        append_headers(out, resp.headers);
        out += resp.body;
        return out;
    }

    std::string_view reason_phrase(int status) {
        switch (status) {
            case 100: return "Continue";
            case 101: return "Switching Protocols";
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
            case 203: return "Non-Authoritative Information";
            case 204: return "No Content";
            case 206: return "Partial Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 303: return "See Other";
            case 304: return "Not Modified";
            case 307: return "Temporary Redirect";
            case 308: return "Permanent Redirect";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 408: return "Request Timeout";
            case 410: return "Gone";
            case 411: return "Length Required";
            case 413: return "Content Too Large";
            case 414: return "URI Too Long";
            case 417: return "Expectation Failed";
            case 431: return "Request Header Fields Too Large";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            case 504: return "Gateway Timeout";
            case 505: return "HTTP Version Not Supported";
            case 508: return "Loop Detected";
            default: return "Unknown";
        }
    }

    bool response_has_no_body(std::string_view request_method, int status) {
        if (request_method == "HEAD") return true;
        if (status >= 100 && status < 200) return true;
        return status == 204 || status == 205 || status == 304;
    }

    void remove_hop_by_hop(Headers& headers) {
        for (const auto& name : headers.get_tokens("Connection")) headers.remove(name);
        static constexpr std::string_view kHopByHop[] = {
            "Connection",
            "Keep-Alive",
            "Proxy-Connection",
            "Proxy-Authenticate",
            "Proxy-Authorization",
            "TE",
            "Trailer",
            "Transfer-Encoding",
            "Upgrade",
        };
        for (auto name : kHopByHop) headers.remove(name);
    }

    bool wants_keep_alive(const Request& req) {
        auto tokens = req.headers.get_tokens("Connection");
        auto proxy_tokens = req.headers.get_tokens("Proxy-Connection");
        tokens.insert(tokens.end(), proxy_tokens.begin(), proxy_tokens.end());
        auto has = [&](std::string_view t) { return std::find(tokens.begin(), tokens.end(), t) != tokens.end(); };

        if (has("close")) return false;
        if (req.version.major == 1 && req.version.minor >= 1) return true;
        return has("keep-alive");
    }

}  // namespace proxy::http