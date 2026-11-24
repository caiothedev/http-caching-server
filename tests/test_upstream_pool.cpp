#include <chrono>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>

#include "proxy/upstream_pool.hpp"

namespace {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    // A connected pair of sockets: `client` would be the proxy side.
    struct Pair {
        asio::io_context io;
        tcp::acceptor acceptor{io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
        tcp::socket client{io};
        tcp::socket server{io};

        Pair() {
            client.connect(acceptor.local_endpoint());
            acceptor.accept(server);
        }
    };

}  // namespace

TEST_CASE("a released connection can be acquired again for the same key", "[pool]") {
    Pair pair;
    proxy::UpstreamPool pool(4, std::chrono::seconds{30});
    pool.release("origin:80", std::move(pair.client));
    CHECK(pool.idle() == 1);

    CHECK_FALSE(pool.acquire("other:80", pair.io.get_executor()));
    auto socket = pool.acquire("origin:80", pair.io.get_executor());
    REQUIRE(socket);
    CHECK(pool.idle() == 0);
    CHECK(pool.reused() == 1);

    // Still the same TCP connection.
    asio::write(*socket, asio::buffer(std::string("ping")));
    char buffer[4];
    asio::read(pair.server, asio::buffer(buffer));
    CHECK(std::string(buffer, 4) == "ping");
}

TEST_CASE("connections closed by the peer or with unread data are discarded", "[pool]") {
    proxy::UpstreamPool pool(4, std::chrono::seconds{30});

    Pair closed;
    pool.release("origin:80", std::move(closed.client));
    closed.server.close();

    Pair chatty;
    pool.release("origin:81", std::move(chatty.client));
    asio::write(chatty.server, asio::buffer(std::string("unexpected")));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(pool.acquire("origin:80", closed.io.get_executor()));
    CHECK_FALSE(pool.acquire("origin:81", chatty.io.get_executor()));
    CHECK(pool.idle() == 0);
}

TEST_CASE("idle connections expire and the pool size is bounded", "[pool]") {
    SECTION("expiry") {
        Pair pair;
        proxy::UpstreamPool pool(4, std::chrono::seconds{1});
        pool.release("origin:80", std::move(pair.client));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        CHECK_FALSE(pool.acquire("origin:80", pair.io.get_executor()));
    }
    SECTION("bound: the oldest connection is dropped") {
        Pair a, b, c;
        proxy::UpstreamPool pool(2, std::chrono::seconds{30});
        pool.release("a:80", std::move(a.client));
        pool.release("b:80", std::move(b.client));
        pool.release("c:80", std::move(c.client));
        CHECK(pool.idle() == 2);
        CHECK_FALSE(pool.acquire("a:80", a.io.get_executor()));
        CHECK(pool.acquire("c:80", c.io.get_executor()));
    }
    SECTION("disabled") {
        Pair pair;
        proxy::UpstreamPool pool(0, std::chrono::seconds{30});
        CHECK_FALSE(pool.enabled());
        pool.release("origin:80", std::move(pair.client));
        CHECK(pool.idle() == 0);
    }
}