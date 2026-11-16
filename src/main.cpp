#include <algorithm>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include "proxy/config.hpp"
#include "proxy/server.hpp"
#include "util/logger.hpp"

int main(int argc, char** argv) {
    auto outcome = proxy::parse_command_line(argc, argv);
    
    if (!outcome.config) {
        (outcome.exit_code == 0 ? std::cout : std::cerr) << outcome.message;
        return outcome.exit_code;
    }

    auto config = *outcome.config;
    proxy::log::set_level(config.log_level);

    unsigned threads = config.threads != 0 ? config.threads : std::max(1u, std::thread::hardware_concurrency());

    try {
        boost::asio::io_context io(static_cast<int>(threads));
        auto ctx = std::make_shared<proxy::ProxyContext>(config);
        proxy::Server server(io, ctx);
        server.start();

        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code&, int sig) {
            LOG_INFO("received signal " << sig << ", shutting down");
            server.stop();
            io.stop();
        });

        auto ep = server.local_endpoint();
        LOG_INFO("proxy listening on " << ep.address().to_string() << ":" << ep.port() << " with " << threads
                                       << " threads, cache "
                                       << (config.cache_enabled
                                               ? std::to_string(config.cache.max_bytes / (1024 * 1024)) + " MiB"
                                               : std::string("disabled")));

        // Thread pool: every thread runs the same io_context and picks up
        // whichever handlers are ready.
        std::vector<std::thread> pool;
        pool.reserve(threads - 1);
        for (unsigned i = 1; i < threads; ++i) pool.emplace_back([&io] { io.run(); });
        io.run();
        for (auto& t : pool) t.join();

        auto stats = ctx->cache.stats();
        LOG_INFO("cache stats: " << stats.hits << " hits, " << stats.misses << " misses, " << stats.revalidations
                                 << " revalidations, " << stats.entries << " entries");
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: " << e.what());
        return 1;
    }

    return 0;
}