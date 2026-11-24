#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/ip/tcp.hpp>

#include "cache/http_cache.hpp"
#include "http/parser.hpp"
#include "http/url.hpp"
#include "proxy/deadline.hpp"
#include "proxy/server.hpp"
#include "proxy/upstream.hpp"

namespace proxy {

    class Tunnel;

    // One client connection. Reads requests (keep-alive and pipelining aware),
    // answers from the cache or forwards them upstream, and turns into a Tunnel
    // on CONNECT.
    //
    // Every handler of a session runs on the session's strand, so its members
    // need no locking. Lifetime is managed by shared_from_this(): each pending
    // asynchronous operation holds a reference.
    class Session : public std::enable_shared_from_this<Session> {
    public:
        Session(boost::asio::ip::tcp::socket socket, std::shared_ptr<ProxyContext> ctx);
        ~Session();

        void start();
        void shutdown();
        void set_tunnel_handler(std::function<void(std::shared_ptr<Tunnel>)> handler) {
            tunnel_handler_ = std::move(handler);
        }

    private:
        void do_read();
        void on_read(const boost::system::error_code& ec, std::size_t n);
        void process();
        void arm_shutdown_grace();
        bool busy() const { return upstream_ != nullptr || writing_ || connecting_; }

        void handle_request(http::Request req);
        void handle_local(const http::Request& req);
        void handle_connect(const http::Request& req);
        void start_tunnel();

        void forward(http::Request req, cache::EntryPtr stale);
        void on_upstream_head(http::Response resp, bool complete);
        void on_upstream_complete(http::Response resp);
        void on_upstream_body(std::string data, bool last);
        void on_upstream_error(UpstreamError error);
        void invalidate_after_unsafe(const http::Response& resp);
        std::string_view cache_status_for_miss() const;
        void serve_entry(const cache::EntryPtr& entry, std::string_view cache_status);

        // Streaming a response whose body is still arriving from the origin.
        void start_stream(http::Response resp);
        void write_stream(std::string head, std::string data, bool last);
        void on_stream_write(const boost::system::error_code& ec);
        void store_streamed_response();

        void prepare_head(http::Response& resp, std::string_view cache_status);
        void log_access(int status, std::uint64_t body_bytes, std::string_view cache_status);
        void send_response(http::Response resp, std::string_view cache_status = {});
        void write_response(http::Response head, std::string_view body, std::string_view cache_status);
        void send_error(int status, const std::string& message);
        void on_write(const boost::system::error_code& ec);
        void after_response();
        void close();

        boost::asio::ip::tcp::socket client_;
        std::shared_ptr<ProxyContext> ctx_;
        Deadline deadline_;
        Deadline shutdown_grace_;
        bool grace_armed_ = false;
        std::uint64_t id_;
        std::string peer_;

        std::array<char, 16 * 1024> buffer_{};
        std::string pending_;  // bytes read from the client but not parsed yet
        bool reading_ = false;  // an async_read_some on client_ is outstanding
        bool writing_ = false;  // an async_write on client_ is outstanding
        http::RequestParser parser_;
        bool continue_sent_ = false;

        // State of the request being served.
        http::Request current_;  // body moved out once forwarded
        std::optional<http::Url> url_;
        std::string cache_key_;
        bool keep_alive_ = false;
        cache::Clock::time_point request_time_;
        cache::Clock::time_point response_time_;
        cache::EntryPtr stale_;
        std::chrono::steady_clock::time_point started_;
        // Response being written. The body is not copied into a write buffer:
        // it is referenced from write_resp_, serving_ (a cache entry) or stream_data_.
        std::string write_head_;
        http::Response write_resp_;
        cache::EntryPtr serving_;

        // Streaming state.
        bool streaming_ = false;
        bool stream_chunked_ = false;  // re-chunking for the client
        bool stream_done_ = false;     // the last piece is being written
        bool stream_cacheable_ = false;
        int stream_status_ = 0;
        std::uint64_t stream_bytes_ = 0;
        std::string stream_cache_status_;
        std::string stream_data_;
        std::string stream_suffix_;
        http::Response stream_store_;
        std::shared_ptr<UpstreamExchange> upstream_;
        bool shutting_down_ = false;
        std::function<void(std::shared_ptr<Tunnel>)> tunnel_handler_;

        // CONNECT handling.
        std::unique_ptr<boost::asio::ip::tcp::resolver> resolver_;
        std::unique_ptr<boost::asio::ip::tcp::socket> tunnel_socket_;
        std::vector<boost::asio::ip::tcp::endpoint> connect_endpoints_;
        bool connecting_ = false;
        bool tunnel_pending_ = false;  // tunnel waits for an outstanding read to finish

        bool connection_slot_transferred_ = false;
        bool closed_ = false;
    };

}  // namespace proxy