#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "cache/http_cache.hpp"
#include "proxy/config.hpp"

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
    };

    // State shared by every session. HttpCache is internally synchronized and the
    // stats are atomics, so sessions on different threads can use it freely.
    struct ProxyContext {
        explicit ProxyContext(Config cfg) : config(std::move(cfg)), cache(config.cache) {}

        Config config;
        cache::HttpCache cache;
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

        boost::asio::io_context& io_;
        boost::asio::ip::tcp::acceptor acceptor_;
        std::shared_ptr<ProxyContext> ctx_;
        std::mutex mutex_;
        bool stopping_ = false;
        std::vector<std::weak_ptr<Session>> sessions_;
        std::vector<std::weak_ptr<Tunnel>> tunnels_;
    };

}  // namespace proxy