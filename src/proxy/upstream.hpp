#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/ip/tcp.hpp>

#include "http/parser.hpp"
#include "proxy/deadline.hpp"

namespace proxy {

    class UpstreamPool;

    struct UpstreamError {
        bool timed_out = false;
        bool forbidden = false;  // every resolved address was refused by the access policy
        std::string message;
    };

    struct UpstreamOptions {
        std::chrono::seconds timeout{30};
        http::ParserLimits limits;
        bool block_private = false;
        // Keep-alive pool; nullptr (or a disabled pool) means a fresh
        // connection with "Connection: close" semantics for every exchange.
        UpstreamPool* pool = nullptr;
        // Whether an idle pooled connection may carry this request. Only
        // idempotent requests are sent on reused connections, because they can
        // be retried safely when the origin closed the connection meanwhile.
        bool reuse_allowed = false;
    };

    // One request/response exchange with an origin server, streamed:
    //
    //   resolve -> connect (or reuse a pooled connection) -> write request
    //   -> on_head(...) -> [resume() -> on_body(...)]* -> done
    //
    // When the response arrives complete with its headers (small bodies, HEAD,
    // 304...), on_head() receives everything at once. Otherwise on_head()
    // receives the headers and the first body bytes, and the exchange pauses:
    // nothing more is read from the origin until the caller calls resume(),
    // typically after writing what it got to the client. That one-chunk-at-a-time
    // hand-off is the backpressure: a slow client slows the origin down instead
    // of making the proxy buffer the response.
    //
    // Deadlines: `timeout` bounds the time until the response headers arrive
    // (absolute) and every wait for origin bytes (inactivity); the inactivity
    // deadline is suspended while paused. A long download that keeps making
    // progress is therefore not cut off.
    //
    // After a complete response that allows it, the connection goes back to the
    // pool. A failure on a reused connection before any response byte arrived
    // is retried once on a fresh connection.
    //
    // All handlers run on the executor passed to the constructor.
    class UpstreamExchange : public std::enable_shared_from_this<UpstreamExchange> {
    public:
        struct Handlers {
            // `response` holds the headers and the body received so far; when
            // `complete` the body is whole and the exchange is finished.
            std::function<void(http::Response response, bool complete)> on_head;
            // Decoded body bytes. `last` marks the end of the response.
            std::function<void(std::string data, bool last)> on_body;
            std::function<void(UpstreamError error)> on_error;
        };

        UpstreamExchange(const boost::asio::any_io_executor& ex, std::string host, std::uint16_t port,
                         std::string request_bytes, std::string request_method, UpstreamOptions options,
                         Handlers handlers);

        void start();
        // Continues reading after on_head(..., false) or on_body(..., false).
        void resume();
        // Aborts the exchange (e.g. the client went away). No handler is called.
        void cancel();

        bool reused_connection() const { return reused_; }

    private:
        void connect_fresh();
        void on_resolve(const boost::system::error_code& ec,
                        const boost::asio::ip::tcp::resolver::results_type& results);
        void on_connect(const boost::system::error_code& ec);
        void write_request();
        void on_write(const boost::system::error_code& ec);
        void do_read();
        void on_read(const boost::system::error_code& ec, std::size_t n);
        void deliver();
        void pause();
        bool retry_on_fresh_connection();
        void fail(const std::string& what, const boost::system::error_code& ec);
        void on_timeout(const char* reason);
        void report_error(UpstreamError error);
        void finish(bool reuse);
        bool response_allows_reuse(const http::Response& head) const;

        boost::asio::any_io_executor ex_;
        boost::asio::ip::tcp::resolver resolver_;
        boost::asio::ip::tcp::socket socket_;
        Deadline total_deadline_;
        Deadline idle_deadline_;
        std::string host_;
        std::uint16_t port_;
        std::string pool_key_;
        std::string request_bytes_;
        std::string request_method_;
        UpstreamOptions options_;
        std::vector<boost::asio::ip::tcp::endpoint> endpoints_;
        http::ResponseParser parser_;
        std::array<char, 16 * 1024> buffer_{};
        Handlers handlers_;

        std::uint64_t bytes_received_ = 0;
        bool reused_ = false;  // the current connection came from the pool
        bool retried_ = false;
        bool head_delivered_ = false;
        bool reusable_ = false;        // connection may go back to the pool when done
        bool trailing_bytes_ = false;  // bytes after the response: connection out of sync
        bool paused_ = false;
        bool timed_out_ = false;
        bool done_ = false;
    };

}  // namespace proxy