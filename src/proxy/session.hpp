#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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

        void handle_request(http::Request req);
        void handle_local(const http::Request& req);
        void handle_connect(const http::Request& req);
        void start_tunnel();

        void forward(http::Request req, cache::EntryPtr stale);
        void on_upstream(UpstreamResult result, const cache::EntryPtr& stale);
        void serve_entry(const cache::EntryPtr& entry, std::string_view cache_status);

        void send_response(http::Response resp, std::string_view cache_status = {});
        void send_error(int status, const std::string& message);
        void on_write(const boost::system::error_code& ec);
        void close();

        boost::asio::ip::tcp::socket client_;
        std::shared_ptr<ProxyContext> ctx_;
        Deadline deadline_;
        std::uint64_t id_;
        std::string peer_;

        std::array<char, 16 * 1024> buffer_{};
        std::string pending_;  // bytes read from the client but not parsed yet
        http::RequestParser parser_;
        bool continue_sent_ = false;

        // State of the request being served.
        http::Request current_;  // body moved out once forwarded
        std::optional<http::Url> url_;
        std::string cache_key_;
        bool keep_alive_ = false;
        cache::Clock::time_point request_time_;
        std::chrono::steady_clock::time_point started_;
        std::string write_buffer_;
        std::shared_ptr<UpstreamExchange> upstream_;
        bool shutting_down_ = false;
        std::function<void(std::shared_ptr<Tunnel>)> tunnel_handler_;

        // CONNECT handling.
        std::unique_ptr<boost::asio::ip::tcp::resolver> resolver_;
        std::unique_ptr<boost::asio::ip::tcp::socket> tunnel_socket_;

        bool closed_ = false;
    };

}  // namespace proxy