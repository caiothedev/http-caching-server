#include "proxy/upstream.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/write.hpp>

#include "util/logger.hpp"

namespace proxy {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    UpstreamExchange::UpstreamExchange(const asio::any_io_executor& ex, std::string host, std::uint16_t port,
                                    std::string request_bytes, std::string request_method,
                                    std::chrono::seconds timeout, http::ParserLimits limits,
                                    Handler handler)
        : resolver_(ex),
        socket_(ex),
        total_deadline_(ex),
        idle_deadline_(ex),
        host_(std::move(host)),
        port_(port),
        request_bytes_(std::move(request_bytes)),
        timeout_(timeout),
        parser_(std::move(request_method), limits),
        handler_(std::move(handler)) {}

    void UpstreamExchange::start() {
        auto self = shared_from_this();
        total_deadline_.start(timeout_, [self] { self->on_timeout("total duration"); });
        idle_deadline_.start(timeout_, [self] { self->on_timeout("inactivity"); });
        resolver_.async_resolve(host_, std::to_string(port_),
                                [self](const boost::system::error_code& ec, tcp::resolver::results_type r) {
                                    self->on_resolve(ec, r);
                                });
    }

    void UpstreamExchange::cancel() {
        done_ = true;
        handler_ = nullptr;
        total_deadline_.stop();
        idle_deadline_.stop();
        resolver_.cancel();
        boost::system::error_code ignored;
        socket_.close(ignored);
    }

    void UpstreamExchange::on_resolve(const boost::system::error_code& ec,
                                    const tcp::resolver::results_type& results) {
        if (done_) return;
        if (ec) return fail("resolve " + host_, ec);
        idle_deadline_.extend(timeout_);
        asio::async_connect(socket_, results,
                            [self = shared_from_this()](const boost::system::error_code& e, const tcp::endpoint&) {
                                self->on_connect(e);
                            });
    }

    void UpstreamExchange::on_connect(const boost::system::error_code& ec) {
        if (done_) return;
        if (ec) return fail("connect " + host_ + ":" + std::to_string(port_), ec);
        idle_deadline_.extend(timeout_);
        asio::async_write(socket_, asio::buffer(request_bytes_),
                        [self = shared_from_this()](const boost::system::error_code& e, std::size_t) {
                            self->on_write(e);
                        });
    }

    void UpstreamExchange::on_write(const boost::system::error_code& ec) {
        if (done_) return;
        if (ec) return fail("write to " + host_, ec);
        idle_deadline_.extend(timeout_);
        request_bytes_.clear();
        request_bytes_.shrink_to_fit();
        do_read();
    }

    void UpstreamExchange::do_read() {
        if (done_) return;
        socket_.async_read_some(asio::buffer(buffer_),
                                [self = shared_from_this()](const boost::system::error_code& ec, std::size_t n) {
                                    self->on_read(ec, n);
                                });
    }

    void UpstreamExchange::on_read(const boost::system::error_code& ec, std::size_t n) {
        if (done_) return;
        if (n > 0) {
            idle_deadline_.extend(timeout_);
            parser_.feed(std::string_view(buffer_.data(), n));
        }

        if (parser_.status() == http::ParseStatus::NeedMore) {
            if (ec == asio::error::eof) {
                parser_.finish();  // completes "read until close" bodies
            } else if (ec) {
                return fail("read from " + host_, ec);
            } else {
                return do_read();
            }
        }

        if (parser_.status() == http::ParseStatus::Error) {
            UpstreamResult result;
            result.error = "invalid response from " + host_ + ": " + parser_.error_message();
            return finish(std::move(result));
        }

        UpstreamResult result;
        result.ok = true;
        result.response = parser_.take();
        finish(std::move(result));
    }

    void UpstreamExchange::fail(const std::string& what, const boost::system::error_code& ec) {
        UpstreamResult result;
        result.timed_out = timed_out_;
        result.error = timed_out_ ? what + ": timed out" : what + ": " + ec.message();
        finish(std::move(result));
    }

    void UpstreamExchange::on_timeout(const char* reason) {
        if (done_) return;
        timed_out_ = true;
        UpstreamResult result;
        result.timed_out = true;
        result.error = "upstream " + std::string(reason) + " timeout for " + host_;
        finish(std::move(result));
    }

    void UpstreamExchange::finish(UpstreamResult result) {
        if (done_) return;
        done_ = true;
        total_deadline_.stop();
        idle_deadline_.stop();
        resolver_.cancel();
        boost::system::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
        auto handler = std::move(handler_);
        handler_ = nullptr;
        if (handler) handler(std::move(result));
    }

}  // namespace proxy