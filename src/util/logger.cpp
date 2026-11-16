#include "util/logger.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <functional>
#include <thread>

#include "util/strings.hpp"

namespace proxy::log {

    namespace {

        std::atomic<int> g_level{static_cast<int>(Level::Info)};
        std::mutex g_mutex;

        const char* level_name(Level level) {
            switch (level) {
                case Level::Debug: return "DEBUG";
                case Level::Info: return "INFO ";
                case Level::Warn: return "WARN ";
                case Level::Error: return "ERROR";
            }

            return "?";
        }
        
    }  // namespace

    void set_level(Level level) { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }

    bool enabled(Level level) { return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed); }

    void write(Level level, std::string_view message) {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto t = system_clock::to_time_t(now);
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        std::tm tm{};
        localtime_r(&t, &tm);

        std::ostringstream line;
        line << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
            << ms.count() << " [" << level_name(level) << "] [t" << std::hash<std::thread::id>{}(std::this_thread::get_id()) % 10000
            << "] " << message << '\n';

        std::lock_guard<std::mutex> lock(g_mutex);
        auto& out = (level >= Level::Warn) ? std::cerr : std::clog;
        out << line.str() << std::flush;
    }

    std::optional<Level> parse_level(std::string_view name) {
        auto n = util::to_lower(name);
        
        if (n == "debug") return Level::Debug;
        if (n == "info") return Level::Info;
        if (n == "warn" || n == "warning") return Level::Warn;
        if (n == "error") return Level::Error;

        return std::nullopt;
    }

}  // namespace proxy::log