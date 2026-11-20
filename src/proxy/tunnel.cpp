#include "proxy/tunnel.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

#include "util/logger.hpp"

namespace proxy {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    Tunnel::Tunnel(tcp::socket client, tcp::socket server, std::chrono::seconds idle_timeout, std::uint64_t id)
        : client_(std::move(client)),
        server_(std::move(server)),
        deadline_(client_.get_executor()),
        idle_timeout_(idle_timeout),
        id_(id) {
        upstream_.from = &client_;
        upstream_.to = &server_;
        downstream_.from = &server_;
        downstream_.to = &client_;
    }

    void Tunnel::start(std::string client_pending) {
        auto self = shared_from_this();
        deadline_.start(idle_timeout_, [self] {
            LOG_DEBUG("[" << self->id_ << "] tunnel idle timeout");
            self->close();
        });

        if (client_pending.empty()) {
            read(upstream_);
            read(downstream_);
            return;
        }
        pending_ = std::move(client_pending);
        upstream_.bytes += pending_.size();
        asio::async_write(server_, asio::buffer(pending_),
                        [self](const boost::system::error_code& ec, std::size_t) {
                            if (ec) return self->close();
                            self->pending_.clear();
                            self->read(self->upstream_);
                            self->read(self->downstream_);
                        });
    }

    void Tunnel::read(Direction& dir) {
        if (closed_) return;
        dir.from->async_read_some(asio::buffer(dir.buffer),
                                [self = shared_from_this(), &dir](const boost::system::error_code& ec,
                                                                    std::size_t n) {
                                    self->on_read(dir, ec, n);
                                });
    }

    void Tunnel::on_read(Direction& dir, const boost::system::error_code& ec, std::size_t n) {
        if (closed_) return;
        if (ec) {
            if (ec == asio::error::eof) return on_eof(dir);
            return close();
        }
        deadline_.extend(idle_timeout_);
        dir.bytes += n;
        asio::async_write(*dir.to, asio::buffer(dir.buffer.data(), n),
                        [self = shared_from_this(), &dir](const boost::system::error_code& e, std::size_t) {
                            if (e) return self->close();
                            self->deadline_.extend(self->idle_timeout_);
                            self->read(dir);
                        });
    }

    void Tunnel::on_eof(Direction& dir) {
        // Half-close: tell the other side no more data is coming in this direction.
        dir.done = true;
        boost::system::error_code ignored;
        dir.to->shutdown(tcp::socket::shutdown_send, ignored);
        if (upstream_.done && downstream_.done) close();
    }

    void Tunnel::shutdown() {
        auto self = shared_from_this();
        asio::post(client_.get_executor(), [self] { self->close(); });
    }

    void Tunnel::close() {
        if (closed_) return;
        closed_ = true;
        deadline_.stop();
        boost::system::error_code ignored;
        client_.shutdown(tcp::socket::shutdown_both, ignored);
        server_.shutdown(tcp::socket::shutdown_both, ignored);
        client_.close(ignored);
        server_.close(ignored);
        LOG_INFO("[" << id_ << "] tunnel closed (" << upstream_.bytes << " bytes up, " << downstream_.bytes
                    << " bytes down)");
    }

}  // namespace proxy