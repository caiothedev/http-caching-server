#include "proxy/server.hpp"

#include <boost/asio/strand.hpp>

#include "proxy/session.hpp"
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
        boost::system::error_code ignored;
        acceptor_.close(ignored);
    }

    void Server::do_accept() {
        // Each connection gets its own strand: all handlers of one session run
        // sequentially, while different sessions run in parallel on the pool.
        acceptor_.async_accept(asio::make_strand(io_), [this](const boost::system::error_code& ec, tcp::socket socket) {
            if (ec == asio::error::operation_aborted) return;  // acceptor closed
            if (ec) {
                LOG_WARN("accept failed: " << ec.message());
            } else {
                std::make_shared<Session>(std::move(socket), ctx_)->start();
            }
            do_accept();
        });
    }

}  // namespace proxy