#include "proxy/upstream.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/write.hpp>

#include "http/url.hpp"
#include "proxy/access.hpp"
#include "proxy/upstream_pool.hpp"
#include "util/logger.hpp"

namespace proxy {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    UpstreamExchange::UpstreamExchange(const asio::any_io_executor& ex, std::string host, std::uint16_t port,
                                        std::string request_bytes, std::string request_method,
                                        UpstreamOptions options, Handlers handlers)
        : ex_(ex),
            resolver_(ex),
            socket_(ex),
            total_deadline_(ex),
            idle_deadline_(ex),
            host_(std::move(host)),
            port_(port),
            pool_key_(http::HostPort{host_, port_}.to_string()),
            request_bytes_(std::move(request_bytes)),
            request_method_(std::move(request_method)),
            options_(options),
            parser_(request_method_, options.limits),
            handlers_(std::move(handlers)) {
        parser_.set_streaming(true);
    }

    void UpstreamExchange::start() {
        auto self = shared_from_this();
        total_deadline_.start(options_.timeout, [self] { self->on_timeout("response headers"); });
        idle_deadline_.start(options_.timeout, [self] { self->on_timeout("inactivity"); });

        if (options_.pool && options_.reuse_allowed) {
            if (auto pooled = options_.pool->acquire(pool_key_, ex_)) {
                socket_ = std::move(*pooled);
                reused_ = true;
                LOG_DEBUG("reusing pooled connection to " << pool_key_);
                return write_request();
            }
        }
        connect_fresh();
    }

    void UpstreamExchange::connect_fresh() {
        resolver_.async_resolve(
            host_, std::to_string(port_),
            [self = shared_from_this()](const boost::system::error_code& ec, tcp::resolver::results_type r) {
                self->on_resolve(ec, r);
            });
    }

    void UpstreamExchange::resume() {
        if (done_ || !paused_) return;
        paused_ = false;
        idle_deadline_.extend(options_.timeout);
        do_read();
    }

    void UpstreamExchange::pause() {
        paused_ = true;
        idle_deadline_.suspend();  // waiting for the client is not origin inactivity
    }

    void UpstreamExchange::cancel() {
        done_ = true;
        handlers_ = {};
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
        AccessPolicy policy;
        policy.block_private = options_.block_private;
        endpoints_ = policy.filter(results);
        if (endpoints_.empty()) {
            UpstreamError error;
            error.forbidden = true;
            error.message = "destination " + host_ + " resolves to a non-public address";
            return report_error(std::move(error));
        }
        idle_deadline_.extend(options_.timeout);
        asio::async_connect(socket_, endpoints_,
                            [self = shared_from_this()](const boost::system::error_code& e,
                                                        const tcp::endpoint&) { self->on_connect(e); });
    }

    void UpstreamExchange::on_connect(const boost::system::error_code& ec) {
        if (done_) return;
        if (ec) return fail("connect " + pool_key_, ec);
        write_request();
    }

    void UpstreamExchange::write_request() {
        idle_deadline_.extend(options_.timeout);
        asio::async_write(socket_, asio::buffer(request_bytes_),
                          [self = shared_from_this()](const boost::system::error_code& e, std::size_t) {
                              self->on_write(e);
                          });
    }

    void UpstreamExchange::on_write(const boost::system::error_code& ec) {
        if (done_) return;
        if (ec) return fail("write to " + host_, ec);
        idle_deadline_.extend(options_.timeout);
        do_read();
    }

    void UpstreamExchange::do_read() {
        if (done_) return;
        socket_.async_read_some(asio::buffer(buffer_),
                                [self = shared_from_this()](const boost::system::error_code& ec,
                                                            std::size_t n) { self->on_read(ec, n); });
    }

    void UpstreamExchange::on_read(const boost::system::error_code& ec, std::size_t n) {
        if (done_) return;
        if (n > 0) {
            bytes_received_ += n;
            idle_deadline_.extend(options_.timeout);
            auto consumed = parser_.feed(std::string_view(buffer_.data(), n));
            // Bytes after the end of the response: the connection is out of sync.
            if (consumed < n && parser_.status() == http::ParseStatus::Complete) trailing_bytes_ = true;
        }

        if (parser_.status() == http::ParseStatus::NeedMore && ec) {
            if (ec == asio::error::eof) {
                if (bytes_received_ == 0 && retry_on_fresh_connection()) return;
                parser_.finish();  // completes "read until close" bodies
            } else {
                return fail("read from " + host_, ec);
            }
        }

        if (parser_.status() == http::ParseStatus::Error) {
            UpstreamError error;
            error.message = "invalid response from " + host_ + ": " + parser_.error_message();
            return report_error(std::move(error));
        }
        deliver();
    }

    void UpstreamExchange::deliver() {
        const bool complete = parser_.status() == http::ParseStatus::Complete;

        if (!head_delivered_) {
            if (!parser_.headers_complete()) return do_read();
            head_delivered_ = true;
            total_deadline_.stop();  // headers arrived in time; the body only has the inactivity limit

            if (complete) {
                auto response = parser_.take();
                bool reuse = response_allows_reuse(response) && !trailing_bytes_;
                auto on_head = std::move(handlers_.on_head);
                finish(reuse);
                if (on_head) on_head(std::move(response), true);
                return;
            }
            auto head = parser_.take_head();
            reusable_ = response_allows_reuse(head);
            head.body = parser_.take_body();
            pause();
            if (handlers_.on_head) handlers_.on_head(std::move(head), false);
            return;
        }

        auto data = parser_.take_body();
        if (complete) {
            auto on_body = std::move(handlers_.on_body);
            finish(reusable_ && !trailing_bytes_);
            if (on_body) on_body(std::move(data), true);
            return;
        }
        if (data.empty()) return do_read();  // e.g. only a chunk-size line arrived
        pause();
        if (handlers_.on_body) handlers_.on_body(std::move(data), false);
    }

    bool UpstreamExchange::response_allows_reuse(const http::Response& head) const {
        if (!options_.pool || !options_.pool->enabled()) return false;
        if (head.version.major != 1 || head.version.minor < 1) return false;
        if (head.headers.has_token("Connection", "close")) return false;
        if (head.status == 101) return false;
        // A body delimited by connection close leaves nothing to reuse.
        return parser_.framing() != http::BodyFraming::UntilClose;
    }

    bool UpstreamExchange::retry_on_fresh_connection() {
        // Only a reused connection that failed before any response byte (the
        // origin closed it while idle) is retried, and only once.
        if (!reused_ || retried_ || bytes_received_ > 0) return false;
        retried_ = true;
        reused_ = false;
        LOG_DEBUG("pooled connection to " << pool_key_ << " was closed by the origin, retrying");
        boost::system::error_code ignored;
        socket_.close(ignored);
        socket_ = tcp::socket(ex_);
        parser_.reset(request_method_);
        connect_fresh();
        return true;
    }

    void UpstreamExchange::fail(const std::string& what, const boost::system::error_code& ec) {
        if (!timed_out_ && retry_on_fresh_connection()) return;
        UpstreamError error;
        error.timed_out = timed_out_;
        error.message = timed_out_ ? what + ": timed out" : what + ": " + ec.message();
        report_error(std::move(error));
    }

    void UpstreamExchange::on_timeout(const char* reason) {
        if (done_) return;
        timed_out_ = true;
        UpstreamError error;
        error.timed_out = true;
        error.message = "upstream " + std::string(reason) + " timeout for " + host_;
        report_error(std::move(error));
    }

    void UpstreamExchange::report_error(UpstreamError error) {
        if (done_) return;
        auto on_error = std::move(handlers_.on_error);
        finish(false);
        if (on_error) on_error(std::move(error));
    }

    void UpstreamExchange::finish(bool reuse) {
        if (done_) return;
        done_ = true;
        total_deadline_.stop();
        idle_deadline_.stop();
        resolver_.cancel();
        if (reuse) {
            options_.pool->release(pool_key_, std::move(socket_));
        } else {
            boost::system::error_code ignored;
            socket_.shutdown(tcp::socket::shutdown_both, ignored);
            socket_.close(ignored);
        }
        handlers_ = {};
    }

}  // namespace proxy