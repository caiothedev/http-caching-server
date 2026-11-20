#include "proxy/config.hpp"

#include <sstream>
#include <string_view>

#include "util/strings.hpp"

namespace proxy {

    namespace {

        bool parse_number(std::string_view s, std::uint64_t min, std::uint64_t max, std::uint64_t& out) {
            return util::parse_uint(s, out) && out >= min && out <= max;
        }

    }  // namespace

    std::string usage(const char* program) {
        std::ostringstream os;
        os << "Usage: " << program << " [options]\n"
        << "\n"
        << "HTTP/1.1 caching proxy (GET/POST/... forwarding, CONNECT tunnelling).\n"
        << "\n"
        << "Options:\n"
        << "  -l, --listen ADDR        address to bind (default 127.0.0.1)\n"
        << "  -p, --port N             port to listen on (default 8080)\n"
        << "  -t, --threads N          worker threads (default: number of CPUs)\n"
        << "      --cache-size MB      total cache size in MiB (default 64)\n"
        << "      --max-object MB      largest cacheable response in MiB (default 8)\n"
        << "      --max-connections N  maximum simultaneous clients (default 16)\n"
        << "      --max-body MB       maximum request/response body in MiB (default 8)\n"
        << "      --no-cache           disable the cache (pure forwarding proxy)\n"
        << "      --timeout SEC        upstream total and inactivity timeout (default 30)\n"
        << "      --idle-timeout SEC   idle keep-alive client timeout (default 60)\n"
        << "      --tunnel-timeout SEC idle CONNECT tunnel timeout (default 300)\n"
        << "      --connect-ports LIST comma separated ports allowed for CONNECT,\n"
        << "                           or 'any' (default 443)\n"
        << "      --log-level LEVEL    debug | info | warn | error (default info)\n"
        << "  -h, --help               show this help\n";
        return os.str();
    }

    ParseOutcome parse_command_line(int argc, char** argv) {
        Config cfg;
        ParseOutcome outcome;
        const char* program = argc > 0 ? argv[0] : "http_proxy";

        auto error = [&](const std::string& msg) {
            outcome.exit_code = 2;
            outcome.message = msg + "\n\n" + usage(program);
            return outcome;
        };

        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            std::string_view value;

            // Accept both "--opt value" and "--opt=value".
            auto eq = arg.find('=');
            bool inline_value = arg.substr(0, 2) == "--" && eq != std::string_view::npos;
            if (inline_value) {
                value = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
            }
            auto next = [&]() -> bool {
                if (inline_value) return true;
                if (i + 1 >= argc) return false;
                value = argv[++i];
                return true;
            };

            std::uint64_t n = 0;
            if (arg == "-h" || arg == "--help") {
                outcome.message = usage(program);
                return outcome;
            } else if (arg == "-l" || arg == "--listen") {
                if (!next() || value.empty()) return error("missing value for --listen");
                cfg.listen_address = std::string(value);
            } else if (arg == "-p" || arg == "--port") {
                if (!next() || !parse_number(value, 1, 65535, n)) return error("invalid --port");
                cfg.port = static_cast<std::uint16_t>(n);
            } else if (arg == "-t" || arg == "--threads") {
                if (!next() || !parse_number(value, 1, 1024, n)) return error("invalid --threads");
                cfg.threads = static_cast<unsigned>(n);
            } else if (arg == "--max-connections") {
                if (!next() || !parse_number(value, 1, 1024, n)) return error("invalid --max-connections");
                cfg.max_connections = static_cast<std::size_t>(n);
            } else if (arg == "--max-body") {
                if (!next() || !parse_number(value, 1, 128, n)) return error("invalid --max-body");
                cfg.limits.max_body_bytes = static_cast<std::size_t>(n) * 1024 * 1024;
            } else if (arg == "--cache-size") {
                if (!next() || !parse_number(value, 1, 1024 * 1024, n)) return error("invalid --cache-size");
                cfg.cache.max_bytes = static_cast<std::size_t>(n) * 1024 * 1024;
            } else if (arg == "--max-object") {
                if (!next() || !parse_number(value, 1, 1024 * 1024, n)) return error("invalid --max-object");
                cfg.cache.max_entry_bytes = static_cast<std::size_t>(n) * 1024 * 1024;
            } else if (arg == "--no-cache") {
                cfg.cache_enabled = false;
            } else if (arg == "--timeout") {
                if (!next() || !parse_number(value, 1, 3600, n)) return error("invalid --timeout");
                cfg.upstream_timeout = std::chrono::seconds(n);
            } else if (arg == "--idle-timeout") {
                if (!next() || !parse_number(value, 1, 3600, n)) return error("invalid --idle-timeout");
                cfg.client_idle_timeout = std::chrono::seconds(n);
            } else if (arg == "--tunnel-timeout") {
                if (!next() || !parse_number(value, 1, 86400, n)) return error("invalid --tunnel-timeout");
                cfg.tunnel_idle_timeout = std::chrono::seconds(n);
            } else if (arg == "--connect-ports") {
                if (!next()) return error("missing value for --connect-ports");
                cfg.connect_ports.clear();
                if (value != "any") {
                    for (auto item : util::split_list(value)) {
                        if (!parse_number(item, 1, 65535, n)) return error("invalid --connect-ports");
                        cfg.connect_ports.push_back(static_cast<std::uint16_t>(n));
                    }
                    if (cfg.connect_ports.empty()) return error("invalid --connect-ports");
                }
            } else if (arg == "--log-level") {
                if (!next()) return error("missing value for --log-level");
                auto level = log::parse_level(value);
                if (!level) return error("invalid --log-level");
                cfg.log_level = *level;
            } else {
                return error("unknown option: " + std::string(arg));
            }
        }

        // Cache entries cannot exceed either the cache capacity or parser body limit.
        if (cfg.cache.max_entry_bytes > cfg.cache.max_bytes) cfg.cache.max_entry_bytes = cfg.cache.max_bytes;
        if (cfg.cache.max_entry_bytes > cfg.limits.max_body_bytes) {
            cfg.cache.max_entry_bytes = cfg.limits.max_body_bytes;
        }

        // Bound aggregate message-body storage across all admitted client sessions.
        constexpr std::size_t kMaxInFlightBodyBytes = 128 * 1024 * 1024;
        if (cfg.max_connections > kMaxInFlightBodyBytes / cfg.limits.max_body_bytes) {
            return error("--max-connections multiplied by --max-body cannot exceed 128 MiB");
        }

        outcome.config = cfg;
        return outcome;
    }

}  // namespace proxy