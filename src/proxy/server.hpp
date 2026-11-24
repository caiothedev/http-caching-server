#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include "cache/http_cache.hpp"
#include "proxy/config.hpp"
#include "proxy/upstream_pool.hpp"

namespace proxy {

    class Session;
    class Tunnel;

    struct ProxyStats {
        std::atomic<std::uint64_t> connections{0};
        std::atomic<std::int64_t> active_connections{0};
        std::atomic<std::int64_t> active_tunnels{0};
        std::atomic<std::uint64_t> requests{0};
        std::atomic<std::uint64_t> tunnels{0};
        std::atomic<std::uint64_t> upstream_errors{0};
        std::atomic<std::uint64_t> client_aborts{0};         // client left while waiting for the origin
        std::atomic<std::uint64_t> blocked_requests{0};      // destination refused by the access policy
        std::atomic<std::uint64_t> rejected_connections{0};  // over --max-connections (503)
        std::atomic<std::int64_t> pending_rejections{0};     // 503 answers still being written
    };

    // State shared by every session. HttpCache is internally synchronized and the
    // stats are atomics, so sessions on different threads can use it freely.
    struct ProxyContext {
        explicit ProxyContext(Config cfg)
            : config(std::move(cfg)), cache(config.cache), pool(config.pool_size, config.pool_idle_timeout) {}

        Config config;
        cache::HttpCache cache;
        UpstreamPool pool;
        ProxyStats stats;
        std::atomic<std::uint64_t> next_session_id{1};
    };

    class Server {
    public:
        Server(boost::asio::io_context& io, std::shared_ptr<ProxyContext> ctx);

        // Binds and starts accepting. Throws boost::system::system_error on failure.
        void start();
        void stop();
        boost::asio::ip::tcp::endpoint local_endpoint() const { return acceptor_.local_endpoint(); }

    private:
        void do_accept();
        void on_accept(const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket);
        void retry_accept_later();
        void reject(boost::asio::ip::tcp::socket socket);

        boost::asio::io_context& io_;
        boost::asio::ip::tcp::acceptor acceptor_;
        std::shared_ptr<ProxyContext> ctx_;
        std::mutex mutex_;
        bool stopping_ = false;
        boost::asio::steady_timer accept_retry_timer_;
        std::chrono::milliseconds accept_backoff_{0};
        std::vector<std::weak_ptr<Session>> sessions_;
        std::vector<std::weak_ptr<Tunnel>> tunnels_;
    };

}  // namespace proxy