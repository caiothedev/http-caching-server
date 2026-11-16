#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace proxy::util {

inline char to_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = to_lower(c);
    return out;
}

inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (to_lower(a[i]) != to_lower(b[i])) return false;
    }
    return true;
}

inline bool is_ows(char c) noexcept { return c == ' ' || c == '\t'; }

// Trims optional whitespace (SP / HTAB) from both ends, as defined in RFC 9110.
inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_ows(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ows(s.back())) s.remove_suffix(1);
    return s;
}

// Splits on `sep`, trimming each element and dropping empty elements.
// Matches the "#rule" list syntax used by most HTTP headers.
inline std::vector<std::string_view> split_list(std::string_view s, char sep = ',') {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find(sep, start);
        if (end == std::string_view::npos) end = s.size();
        auto item = trim(s.substr(start, end - start));
        if (!item.empty()) out.push_back(item);
        start = end + 1;
    }
    return out;
}
// The result holds views into the input, so a temporary string would dangle.
std::vector<std::string_view> split_list(std::string&&, char = ',') = delete;

// Parses a non-empty string of decimal digits. Rejects signs, spaces and overflow.
inline bool parse_uint(std::string_view s, std::uint64_t& out) noexcept {
    if (s.empty()) return false;
    std::uint64_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

// RFC 9110 "tchar": characters allowed in a token (method names, header names...).
inline bool is_tchar(char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

inline bool is_token(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) {
        if (!is_tchar(c)) return false;
    }
    return true;
}

}  // namespace proxy::util