#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio/ip/tcp.hpp>

#include "http/parser.hpp"
#include "proxy/deadline.hpp"

namespace proxy {

    struct UpstreamResult {
        bool ok = false;
        bool timed_out = false;
        std::string error;
        http::Response response;
    };

    // One request/response exchange with an origin server over a fresh TCP
    // connection: resolve -> connect -> write -> read until the response is complete.
    //
    // The request is sent with "Connection: close", so the connection is not
    // reused; connection pooling is a possible future improvement.
    class UpstreamExchange : public std::enable_shared_from_this<UpstreamExchange> {
    public:
        using Handler = std::function<void(UpstreamResult)>;

        UpstreamExchange(const boost::asio::any_io_executor& ex, std::string host, std::uint16_t port,
                        std::string request_bytes, std::string request_method,
                        std::chrono::seconds timeout, http::ParserLimits limits, Handler handler);

        void start();
        // Aborts the exchange (e.g. the client went away). The handler is not called.
        void cancel();

    private:
        void on_resolve(const boost::system::error_code& ec,
                        const boost::asio::ip::tcp::resolver::results_type& results);
        void on_connect(const boost::system::error_code& ec);
        void on_write(const boost::system::error_code& ec);
        void do_read();
        void on_read(const boost::system::error_code& ec, std::size_t n);
        void fail(const std::string& what, const boost::system::error_code& ec);
        void finish(UpstreamResult result);

        boost::asio::ip::tcp::resolver resolver_;
        boost::asio::ip::tcp::socket socket_;
        Deadline deadline_;
        std::string host_;
        std::uint16_t port_;
        std::string request_bytes_;
        std::chrono::seconds timeout_;
        http::ResponseParser parser_;
        std::array<char, 16 * 1024> buffer_{};
        Handler handler_;
        bool timed_out_ = false;
        bool done_ = false;
    };

}  // namespace proxy