#include "proxy/server.hpp"

#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <string>

#include "proxy/session.hpp"
#include "proxy/tunnel.hpp"
#include "util/logger.hpp"

namespace proxy {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    namespace {

        constexpr std::chrono::milliseconds kMinAcceptBackoff{10};
        constexpr std::chrono::milliseconds kMaxAcceptBackoff{1000};
        // How many rejected clients may be waiting for their 503 at once; beyond
        // that, extra connections are simply closed so rejection stays cheap.
        constexpr std::int64_t kMaxPendingRejections = 64;
        constexpr std::chrono::seconds kRejectionLinger{1};

        const std::string kServiceUnavailable = [] {
            const std::string body = "503 Service Unavailable: too many client connections\n";
            return "HTTP/1.1 503 Service Unavailable\r\n"
                   "Content-Type: text/plain; charset=utf-8\r\n"
                   "Cache-Control: no-store\r\n"
                   "Retry-After: 1\r\n"
                   "Connection: close\r\n"
                   "Content-Length: " +
                   std::to_string(body.size()) + "\r\n\r\n" + body;
        }();

        bool try_acquire_connection_slot(const std::shared_ptr<ProxyContext>& ctx) {
            auto active = ctx->stats.active_connections.load();
            const auto limit = static_cast<std::int64_t>(ctx->config.max_connections);
            while (active < limit) {
                if (ctx->stats.active_connections.compare_exchange_weak(active, active + 1)) return true;
            }
            return false;
        }

        // Answers a connection over the limit with 503 and closes it.
        //
        // The client usually has already sent its request. Closing a socket with
        // unread input makes the kernel send RST, which can destroy the 503 before
        // the client reads it, so after writing we half-close and drain input for
        // a short while (until the client closes or the linger timer fires).
        class Rejection : public std::enable_shared_from_this<Rejection> {
        public:
            Rejection(tcp::socket socket, std::shared_ptr<ProxyContext> ctx)
                : socket_(std::move(socket)), timer_(socket_.get_executor()), ctx_(std::move(ctx)) {}
            ~Rejection() { ctx_->stats.pending_rejections--; }

            void start() {
                auto self = shared_from_this();
                timer_.expires_after(kRejectionLinger);
                timer_.async_wait([self](const boost::system::error_code& ec) {
                    if (!ec) self->close();
                });
                asio::async_write(socket_, asio::buffer(kServiceUnavailable),
                                  [self](const boost::system::error_code& ec, std::size_t) {
                                      if (ec) return self->close();
                                      boost::system::error_code ignored;
                                      self->socket_.shutdown(tcp::socket::shutdown_send, ignored);
                                      self->drain();
                                  });
            }

        private:
            void drain() {
                socket_.async_read_some(
                    asio::buffer(buffer_),
                    [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
                        if (ec) return self->close();
                        self->drain();
                    });
            }

            void close() {
                timer_.cancel();
                boost::system::error_code ignored;
                socket_.close(ignored);
            }

            tcp::socket socket_;
            asio::steady_timer timer_;
            std::shared_ptr<ProxyContext> ctx_;
            std::array<char, 4096> buffer_{};
        };

    }  // namespace

    Server::Server(asio::io_context& io, std::shared_ptr<ProxyContext> ctx)
        : io_(io), acceptor_(io), ctx_(std::move(ctx)), accept_retry_timer_(io) {}

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
            accept_retry_timer_.cancel();
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
        acceptor_.async_accept(asio::make_strand(io_),
                               [this](const boost::system::error_code& ec, tcp::socket socket) {
                                   on_accept(ec, std::move(socket));
                               });
    }

    void Server::on_accept(const boost::system::error_code& ec, tcp::socket socket) {
        if (ec == asio::error::operation_aborted) return;  // acceptor closed
        if (ec) {
            // Typically EMFILE/ENFILE/ENOBUFS: retrying at once would fail again
            // immediately and spin. Back off exponentially instead.
            LOG_WARN("accept failed: " << ec.message());
            return retry_accept_later();
        }
        accept_backoff_ = std::chrono::milliseconds{0};

        if (!try_acquire_connection_slot(ctx_)) {
            reject(std::move(socket));
        } else {
            auto session = std::make_shared<Session>(std::move(socket), ctx_);
            session->set_tunnel_handler([this](std::shared_ptr<Tunnel> tunnel) {
                bool reject_tunnel = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    reject_tunnel = stopping_;
                    if (!reject_tunnel) {
                        tunnels_.erase(std::remove_if(tunnels_.begin(), tunnels_.end(),
                                                      [](const auto& item) { return item.expired(); }),
                                       tunnels_.end());
                        tunnels_.push_back(tunnel);
                    }
                }
                if (reject_tunnel) tunnel->shutdown();
            });
            bool accepted = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!stopping_) {
                    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                                   [](const auto& item) { return item.expired(); }),
                                    sessions_.end());
                    sessions_.push_back(session);
                    accepted = true;
                }
            }
            if (accepted) session->start();
            else session->shutdown();
        }
        do_accept();
    }

    void Server::retry_accept_later() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        accept_backoff_ = std::clamp(accept_backoff_ * 2, kMinAcceptBackoff, kMaxAcceptBackoff);
        accept_retry_timer_.expires_after(accept_backoff_);
        accept_retry_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec) do_accept();
        });
    }

    void Server::reject(tcp::socket socket) {
        ctx_->stats.rejected_connections++;
        if (ctx_->stats.pending_rejections.fetch_add(1) >= kMaxPendingRejections) {
            ctx_->stats.pending_rejections--;
            LOG_WARN("maximum client connections reached; closing connection");
            boost::system::error_code ignored;
            socket.close(ignored);
            return;
        }
        LOG_WARN("maximum client connections reached; answering 503");
        std::make_shared<Rejection>(std::move(socket), ctx_)->start();
    }

}  // namespace proxy