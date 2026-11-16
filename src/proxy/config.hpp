#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cache/http_cache.hpp"
#include "http/parser.hpp"
#include "util/logger.hpp"

namespace proxy {

    struct Config {
        std::string listen_address = "127.0.0.1";
        std::uint16_t port = 8080;
        unsigned threads = 0;  // 0 = hardware concurrency

        std::chrono::seconds upstream_timeout{30};
        std::chrono::seconds client_idle_timeout{60};
        std::chrono::seconds tunnel_idle_timeout{300};

        // Ports CONNECT may reach. Empty = any port (not recommended).
        std::vector<std::uint16_t> connect_ports{443};

        bool cache_enabled = true;
        cache::CacheOptions cache;
        http::ParserLimits limits;
        log::Level log_level = log::Level::Info;
    };

    struct ParseOutcome {
        std::optional<Config> config;  // empty when the program should exit
        int exit_code = 0;
        std::string message;           // help text or error
    };

    ParseOutcome parse_command_line(int argc, char** argv);
    std::string usage(const char* program);

}  // namespace proxy