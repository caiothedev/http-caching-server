#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace proxy {

    // Idle keep-alive connections to origin servers, shared by all sessions.
    //
    // Connections are stored as bare file descriptors, detached from any Asio
    // object: a connection opened on one session's strand can be re-wrapped for
    // another session's executor, and the pool never owns pending operations,
    // so it does not keep the io_context running.
    //
    // An idle connection is checked before reuse with a non-blocking MSG_PEEK:
    // a closed connection (the origin's keep-alive timeout) or one with
    // unexpected bytes is discarded. A close racing with reuse is handled by the
    // caller retrying idempotent requests once on a fresh connection.
    class UpstreamPool {
    public:
        using clock = std::chrono::steady_clock;

        // `max_idle` = 0 disables pooling.
        UpstreamPool(std::size_t max_idle, std::chrono::seconds idle_timeout);
        ~UpstreamPool();

        UpstreamPool(const UpstreamPool&) = delete;
        UpstreamPool& operator=(const UpstreamPool&) = delete;

        bool enabled() const { return max_idle_ > 0; }

        // An idle live connection to `key` ("host:port"), bound to `ex`.
        std::optional<boost::asio::ip::tcp::socket> acquire(const std::string& key,
                                                            const boost::asio::any_io_executor& ex);
        // Returns a connection after a complete exchange. It must have no
        // pending operations.
        void release(const std::string& key, boost::asio::ip::tcp::socket socket);

        std::size_t idle() const;
        std::uint64_t reused() const { return reused_.load(); }

    private:
        struct Idle {
            std::string key;
            int fd;
            bool v6;
            clock::time_point since;
        };

        void purge_expired(clock::time_point now);  // mutex_ held

        const std::size_t max_idle_;
        const std::chrono::seconds idle_timeout_;
        mutable std::mutex mutex_;
        std::deque<Idle> idle_;  // oldest first
        std::atomic<std::uint64_t> reused_{0};
    };

}  // namespace proxy