#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <boost/asio/ip/tcp.hpp>

#include "proxy/deadline.hpp"

namespace proxy {

    // Bidirectional byte relay used for CONNECT (typically HTTPS). The proxy does
    // not see or cache the encrypted traffic; it only copies bytes both ways until
    // both sides have finished or the tunnel is idle for too long.
    class Tunnel : public std::enable_shared_from_this<Tunnel> {
    public:
        Tunnel(boost::asio::ip::tcp::socket client, boost::asio::ip::tcp::socket server,
            std::chrono::seconds idle_timeout, std::uint64_t id);

        // `client_pending` holds bytes the client sent right after the CONNECT
        // request (already read from the socket); they are forwarded first.
        void start(std::string client_pending);

    private:
        struct Direction {
            boost::asio::ip::tcp::socket* from;
            boost::asio::ip::tcp::socket* to;
            std::array<char, 16 * 1024> buffer{};
            std::uint64_t bytes = 0;
            bool done = false;
        };

        void read(Direction& dir);
        void on_read(Direction& dir, const boost::system::error_code& ec, std::size_t n);
        void on_eof(Direction& dir);
        void close();

        boost::asio::ip::tcp::socket client_;
        boost::asio::ip::tcp::socket server_;
        Deadline deadline_;
        std::chrono::seconds idle_timeout_;
        std::uint64_t id_;
        Direction upstream_;    // client -> server
        Direction downstream_;  // server -> client
        std::string pending_;
        bool closed_ = false;
    };

}  // namespace proxy