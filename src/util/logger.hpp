#pragma once

#include <optional>
#include <sstream>
#include <string_view>

namespace proxy::log {

    enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

    void set_level(Level level);
    bool enabled(Level level);
    void write(Level level, std::string_view message);
    std::optional<Level> parse_level(std::string_view name);

}  // namespace proxy::log

// Streaming log macros: LOG_INFO("client " << id << " connected");
// The expression is only evaluated when the level is enabled.
#define PROXY_LOG(level, expr)                                  \
    do {                                                        \
        if (::proxy::log::enabled(level)) {                     \
            std::ostringstream proxy_log_os_;                   \
            proxy_log_os_ << expr;                              \
            ::proxy::log::write(level, proxy_log_os_.str());    \
        }                                                       \
    } while (0)

#define LOG_DEBUG(expr) PROXY_LOG(::proxy::log::Level::Debug, expr)
#define LOG_INFO(expr) PROXY_LOG(::proxy::log::Level::Info, expr)
#define LOG_WARN(expr) PROXY_LOG(::proxy::log::Level::Warn, expr)
#define LOG_ERROR(expr) PROXY_LOG(::proxy::log::Level::Error, expr)