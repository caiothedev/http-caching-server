#include "proxy/server.hpp"

#include <boost/asio/strand.hpp>

#include <algorithm>

#include "proxy/session.hpp"
#include "proxy/tunnel.hpp"
#include "util/logger.hpp"

namespace proxy {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    Server::Server(asio::io_context& io, std::shared_ptr<ProxyContext> ctx)
        : io_(io), acceptor_(io), ctx_(std::move(ctx)) {}

    void Server::start() {
        auto address = asio::ip::make_address(ctx_->config.listen_address);
        tcp::endpoint endpoint(address, ctx_->config.port);

        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(asio::socket_base::max_listen_connections);
        do_accept();
    }

    void Server::stop() {
        std::vector<std::weak_ptr<Session>> sessions;
        std::vector<std::weak_ptr<Tunnel>> tunnels;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
            boost::system::error_code ignored;
            acceptor_.close(ignored);
            sessions.swap(sessions_);
            tunnels.swap(tunnels_);
        }

        for (auto& weak : sessions) {
            if (auto session = weak.lock()) session->shutdown();
        }
        for (auto& weak : tunnels) {
            if (auto tunnel = weak.lock()) tunnel->shutdown();
        }
    }

    void Server::do_accept() {
        // Protect accept initiation against a concurrent stop() closing the acceptor.
        std::lock_guard<std::mutex> accept_lock(mutex_);
        if (stopping_) return;

        // Each connection gets its own strand: all handlers of one session run
        // sequentially, while different sessions run in parallel on the pool.
        acceptor_.async_accept(asio::make_strand(io_), [this](const boost::system::error_code& ec, tcp::socket socket) {
            if (ec == asio::error::operation_aborted) return;  // acceptor closed
            if (ec) {
                LOG_WARN("accept failed: " << ec.message());
            } else {
                auto session = std::make_shared<Session>(std::move(socket), ctx_);
                session->set_tunnel_handler([this](std::shared_ptr<Tunnel> tunnel) {
                    bool reject = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        reject = stopping_;
                        if (!reject) {
                            tunnels_.erase(std::remove_if(tunnels_.begin(), tunnels_.end(),
                                [](const auto& item) { return item.expired(); }), tunnels_.end());
                            tunnels_.push_back(tunnel);
                        }
                    }
                    if (reject) tunnel->shutdown();
                });
                bool accept_more = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!stopping_) {
                        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                            [](const auto& item) { return item.expired(); }), sessions_.end());
                        sessions_.push_back(session);
                        accept_more = true;
                    }
                }
                if (accept_more) session->start();
                else session->shutdown();
            }
            bool accept_more = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                accept_more = !stopping_;
            }
            if (accept_more) do_accept();
        });
    }

}  // namespace proxy