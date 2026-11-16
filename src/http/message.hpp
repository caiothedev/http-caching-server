#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace proxy::http {

    struct HeaderField {
        std::string name;
        std::string value;
    };

    // Ordered collection of header fields with case-insensitive name lookup.
    // Order and duplicates are preserved because some headers (e.g. Set-Cookie)
    // cannot be safely combined into one line.
    class Headers {
        public:
            using const_iterator = std::vector<HeaderField>::const_iterator;

            void add(std::string name, std::string value);
            // Replaces every field with this name by a single field.
            void set(std::string name, std::string value);
            // Returns how many fields were removed.
            std::size_t remove(std::string_view name);

            bool contains(std::string_view name) const;
            // Value of the first field with this name.
            std::optional<std::string> get(std::string_view name) const;
            // All values of this name joined with ", " (RFC 9110 §5.3). Empty if absent.
            std::string get_combined(std::string_view name) const;
            // Lowercased, comma-split tokens across all fields with this name.
            std::vector<std::string> get_tokens(std::string_view name) const;
            bool has_token(std::string_view name, std::string_view token) const;

            std::size_t size() const { return fields_.size(); }
            bool empty() const { return fields_.empty(); }
            void clear() { fields_.clear(); }
            const_iterator begin() const { return fields_.begin(); }
            const_iterator end() const { return fields_.end(); }

        private:
            std::vector<HeaderField> fields_;
    };

    struct Version {
        int major = 1;
        int minor = 1;
        bool operator==(const Version& o) const { return major == o.major && minor == o.minor; }
    };

    struct Request {
        std::string method;
        std::string target;
        Version version;
        Headers headers;
        std::string body;
    };

    struct Response {
        Version version;
        int status = 200;
        std::string reason;
        Headers headers;
        std::string body;
    };

    std::string to_string(Version v);
    std::string serialize(const Request& req);
    std::string serialize(const Response& resp);
    std::string_view reason_phrase(int status);

    // True when a response to `request_method` with this status can never carry a body.
    bool response_has_no_body(std::string_view request_method, int status);

    // Removes hop-by-hop headers (RFC 9110 §7.6.1): the fixed list plus any header
    // named in the Connection field.
    void remove_hop_by_hop(Headers& headers);

    // Whether the client wants the connection kept open after this request.
    bool wants_keep_alive(const Request& req);

}  // namespace proxy::http