// End-to-end tests of the proxy running in-process: a real Server on an
// ephemeral port, driven by blocking client sockets, talking to fake origins
// that run on their own threads.

#include <poll.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>

#include "http/parser.hpp"
#include "proxy/server.hpp"
#include "util/logger.hpp"

namespace {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;
    using namespace std::chrono_literals;

    constexpr auto kIoTimeout = 5s;

    // Waits until `socket` is readable. False on timeout.
    bool wait_readable(tcp::socket& socket, std::chrono::milliseconds timeout) {
        pollfd fd{socket.native_handle(), POLLIN, 0};
        return ::poll(&fd, 1, static_cast<int>(timeout.count())) > 0;
    }

    // read_some with a timeout, so a misbehaving proxy fails the test instead of hanging it.
    std::size_t read_some(tcp::socket& socket, std::array<char, 4096>& buffer,
                          boost::system::error_code& ec) {
        if (!wait_readable(socket, std::chrono::duration_cast<std::chrono::milliseconds>(kIoTimeout))) {
            ec = asio::error::timed_out;
            return 0;
        }
        return socket.read_some(asio::buffer(buffer), ec);
    }

    void send(tcp::socket& socket, const std::string& data) { asio::write(socket, asio::buffer(data)); }

    std::optional<proxy::http::Request> read_request(tcp::socket& socket) {
        proxy::http::RequestParser parser;
        std::array<char, 4096> buffer{};
        while (parser.status() == proxy::http::ParseStatus::NeedMore) {
            boost::system::error_code ec;
            auto n = read_some(socket, buffer, ec);
            if (ec) return std::nullopt;
            parser.feed(std::string_view(buffer.data(), n));
        }
        if (parser.status() != proxy::http::ParseStatus::Complete) return std::nullopt;
        return parser.take();
    }

    // Reads one response; bytes after it stay in `leftover` for the next call.
    std::optional<proxy::http::Response> read_response(tcp::socket& socket, std::string& leftover,
                                                       const std::string& method = "GET") {
        proxy::http::ParserLimits limits;
        limits.max_body_bytes = std::size_t{512} << 20;  // the tests stream large bodies
        proxy::http::ResponseParser parser(method, limits);
        auto used = parser.feed(leftover);
        leftover.erase(0, used);
        std::array<char, 4096> buffer{};
        while (parser.status() == proxy::http::ParseStatus::NeedMore) {
            boost::system::error_code ec;
            auto n = read_some(socket, buffer, ec);
            if (ec == asio::error::eof) {
                parser.finish();
                break;
            }
            if (ec) return std::nullopt;
            std::string_view data(buffer.data(), n);
            auto consumed = parser.feed(data);
            leftover.append(data.substr(consumed));
        }
        if (parser.status() != proxy::http::ParseStatus::Complete) return std::nullopt;
        return parser.take();
    }

    std::optional<proxy::http::Response> read_response(tcp::socket& socket) {
        std::string leftover;
        return read_response(socket, leftover);
    }

    // Reads until the end of a header block, returning everything read.
    std::string read_head(tcp::socket& socket) {
        std::string data;
        std::array<char, 4096> buffer{};
        while (data.find("\r\n\r\n") == std::string::npos) {
            boost::system::error_code ec;
            auto n = read_some(socket, buffer, ec);
            if (ec) break;
            data.append(buffer.data(), n);
        }
        return data;
    }

    std::string http_response(int status, const std::string& body, const std::string& extra_headers = "",
                              bool keep_alive = false) {
        return "HTTP/1.1 " + std::to_string(status) + " " + std::string(proxy::http::reason_phrase(status)) +
               "\r\n" + extra_headers + "Content-Length: " + std::to_string(body.size()) + "\r\n" +
               (keep_alive ? "" : "Connection: close\r\n") + "\r\n" + body;
    }

    // A blocking TCP server; each connection is handled on its own thread.
    class FakeOrigin {
    public:
        using RawHandler = std::function<void(tcp::socket&)>;
        using HttpHandler = std::function<void(const proxy::http::Request&, tcp::socket&)>;

        explicit FakeOrigin(RawHandler handler)
            : acceptor_(io_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
              handler_(std::move(handler)) {
            thread_ = std::thread([this] { run(); });
        }

        // Parses one HTTP request per connection before calling `handler`.
        static RawHandler http(HttpHandler handler, std::atomic<int>* counter = nullptr) {
            return [handler = std::move(handler), counter](tcp::socket& socket) {
                auto request = read_request(socket);
                if (!request) return;
                if (counter) ++*counter;
                handler(*request, socket);
            };
        }

        // Serves requests on each connection until the proxy closes it.
        static RawHandler keep_alive(HttpHandler handler, std::atomic<int>* connections,
                                     std::atomic<int>* requests) {
            return [handler = std::move(handler), connections, requests](tcp::socket& socket) {
                if (connections) ++*connections;
                while (auto request = read_request(socket)) {
                    if (requests) ++*requests;
                    handler(*request, socket);
                    if (!socket.is_open()) return;
                }
            };
        }

        ~FakeOrigin() {
            stopping_ = true;
            boost::system::error_code ignored;
            tcp::socket wake(io_);
            wake.connect(acceptor_.local_endpoint(), ignored);  // unblocks accept()
            thread_.join();
            for (auto& t : handlers_) t.join();
        }

        std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
        std::string url(const std::string& path) const {
            return "http://127.0.0.1:" + std::to_string(port()) + path;
        }

    private:
        void run() {
            for (;;) {
                tcp::socket socket(io_);
                boost::system::error_code ec;
                acceptor_.accept(socket, ec);
                if (stopping_) return;
                if (ec) continue;
                handlers_.emplace_back(
                    [this, s = std::make_shared<tcp::socket>(std::move(socket))] { handler_(*s); });
            }
        }

        asio::io_context io_;
        tcp::acceptor acceptor_;
        RawHandler handler_;
        std::atomic<bool> stopping_{false};
        std::thread thread_;
        std::vector<std::thread> handlers_;  // only touched by thread_ until it is joined
    };

    void echo(tcp::socket& socket) {
        std::array<char, 4096> buffer{};
        for (;;) {
            boost::system::error_code ec;
            auto n = read_some(socket, buffer, ec);
            if (ec) return;
            asio::write(socket, asio::buffer(buffer.data(), n), ec);
            if (ec) return;
        }
    }

    // The proxy under test, running on two threads.
    class ProxyHarness {
    public:
        explicit ProxyHarness(proxy::Config config = {}) {
            proxy::log::set_level(proxy::log::Level::Error);
            config.listen_address = "127.0.0.1";
            config.port = 0;
            ctx = std::make_shared<proxy::ProxyContext>(config);
            server_ = std::make_unique<proxy::Server>(io_, ctx);
            server_->start();
            port = server_->local_endpoint().port();
            for (int i = 0; i < 2; ++i) threads_.emplace_back([this] { io_.run(); });
        }

        ~ProxyHarness() { stop(); }

        void stop() {
            if (stopped_.exchange(true)) return;
            asio::post(io_, [this] { server_->stop(); });
            for (auto& t : threads_) t.join();
        }

        tcp::socket connect() {
            tcp::socket socket(client_io_);
            socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
            return socket;
        }

        std::shared_ptr<proxy::ProxyContext> ctx;
        std::uint16_t port = 0;

    private:
        asio::io_context io_;
        asio::io_context client_io_;
        std::unique_ptr<proxy::Server> server_;
        std::vector<std::thread> threads_;
        std::atomic<bool> stopped_{false};
    };

    std::string get(const std::string& url, const std::string& extra = "") {
        auto authority = url.substr(7, url.find('/', 7) - 7);
        return "GET " + url + " HTTP/1.1\r\nHost: " + authority + "\r\n" + extra + "\r\n";
    }

    std::string header(const proxy::http::Response& resp, const char* name) {
        return resp.headers.get(name).value_or("");
    }

}  // namespace

TEST_CASE("a cacheable response is served from the cache the second time", "[proxy]") {
    std::atomic<int> hits{0};
    FakeOrigin origin(FakeOrigin::http(
        [](const proxy::http::Request&, tcp::socket& s) {
            send(s, http_response(200, "hello", "Cache-Control: max-age=60\r\n"));
        },
        &hits));
    ProxyHarness proxy;

    for (const char* expected : {"MISS", "HIT"}) {
        auto client = proxy.connect();
        send(client, get(origin.url("/cached")));
        auto resp = read_response(client);
        REQUIRE(resp);
        CHECK(resp->status == 200);
        CHECK(resp->body == "hello");
        CHECK(header(*resp, "X-Cache") == expected);
    }
    CHECK(hits == 1);
}

TEST_CASE("a stale entry is revalidated with a conditional request", "[proxy]") {
    FakeOrigin origin(FakeOrigin::http([](const proxy::http::Request& req, tcp::socket& s) {
        const std::string headers = "Cache-Control: max-age=0\r\nETag: \"v1\"\r\n";
        if (req.headers.get("If-None-Match") == std::optional<std::string>("\"v1\"")) {
            send(s, "HTTP/1.1 304 Not Modified\r\n" + headers + "Connection: close\r\n\r\n");
        } else {
            send(s, http_response(200, "original", headers));
        }
    }));
    ProxyHarness proxy;

    auto first = proxy.connect();
    send(first, get(origin.url("/etag")));
    REQUIRE(read_response(first));

    auto second = proxy.connect();
    send(second, get(origin.url("/etag")));
    auto resp = read_response(second);
    REQUIRE(resp);
    CHECK(resp->status == 200);
    CHECK(resp->body == "original");
    CHECK(header(*resp, "X-Cache") == "REVALIDATED");
}

TEST_CASE("pipelined requests are answered in order", "[proxy]") {
    FakeOrigin origin(FakeOrigin::http([](const proxy::http::Request& req, tcp::socket& s) {
        std::this_thread::sleep_for(50ms);  // the second request arrives while the first is upstream
        send(s, http_response(200, req.target, "Cache-Control: no-store\r\n"));
    }));
    ProxyHarness proxy;

    auto client = proxy.connect();
    send(client, get(origin.url("/first")) + get(origin.url("/second")) + get(origin.url("/third")));
    std::string leftover;
    for (const char* path : {"/first", "/second", "/third"}) {
        auto resp = read_response(client, leftover);
        REQUIRE(resp);
        CHECK(resp->body == path);
    }
}

TEST_CASE("Expect: 100-continue is answered before the body is sent", "[proxy]") {
    FakeOrigin origin(FakeOrigin::http([](const proxy::http::Request& req, tcp::socket& s) {
        CHECK_FALSE(req.headers.contains("Expect"));
        send(s, http_response(200, req.body));
    }));
    ProxyHarness proxy;

    auto client = proxy.connect();
    auto url = origin.url("/upload");
    send(client, "POST " + url + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(origin.port()) +
                     "\r\nContent-Length: 7\r\nExpect: 100-continue\r\n\r\n");
    auto interim = read_head(client);
    CHECK(interim.rfind("HTTP/1.1 100 Continue\r\n", 0) == 0);

    send(client, "payload");
    auto resp = read_response(client);
    REQUIRE(resp);
    CHECK(resp->status == 200);
    CHECK(resp->body == "payload");
}

TEST_CASE("a client that disconnects cancels its upstream request", "[proxy]") {
    std::promise<void> got_request;
    std::promise<bool> upstream_closed;
    FakeOrigin origin(FakeOrigin::http([&](const proxy::http::Request&, tcp::socket& s) {
        got_request.set_value();
        // Never answer; report whether the proxy closes the connection in time.
        bool closed = false;
        if (wait_readable(s, 3000ms)) {
            std::array<char, 16> buffer{};
            boost::system::error_code ec;
            s.read_some(asio::buffer(buffer), ec);
            closed = ec == asio::error::eof || ec == asio::error::connection_reset;
        }
        upstream_closed.set_value(closed);
    }));
    ProxyHarness proxy;

    {
        auto client = proxy.connect();
        send(client, get(origin.url("/slow")));
        REQUIRE(got_request.get_future().wait_for(kIoTimeout) == std::future_status::ready);
    }  // client closes here

    auto closed = upstream_closed.get_future();
    REQUIRE(closed.wait_for(kIoTimeout) == std::future_status::ready);
    CHECK(closed.get());
    CHECK(proxy.ctx->stats.client_aborts == 1);
}

TEST_CASE("connections over the limit get 503 Service Unavailable", "[proxy]") {
    proxy::Config config;
    config.max_connections = 1;
    ProxyHarness proxy(config);

    // Make sure the first connection owns the only slot.
    auto first = proxy.connect();
    send(first, "GET /__proxy/stats HTTP/1.1\r\nHost: proxy\r\n\r\n");
    REQUIRE(read_response(first));

    auto second = proxy.connect();
    send(second, "GET /__proxy/stats HTTP/1.1\r\nHost: proxy\r\n\r\n");
    auto resp = read_response(second);
    REQUIRE(resp);
    CHECK(resp->status == 503);
    CHECK(header(*resp, "Retry-After") == "1");
    CHECK(proxy.ctx->stats.rejected_connections == 1);
}

TEST_CASE("destinations outside the host allowlist are refused", "[proxy]") {
    std::atomic<int> hits{0};
    FakeOrigin origin(FakeOrigin::http(
        [](const proxy::http::Request&, tcp::socket& s) { send(s, http_response(200, "ok")); }, &hits));
    proxy::Config config;
    config.access.allowed_hosts = {"allowed.test"};
    config.connect_ports = {origin.port()};
    ProxyHarness proxy(config);

    auto client = proxy.connect();
    send(client, get(origin.url("/")));
    auto resp = read_response(client);
    REQUIRE(resp);
    CHECK(resp->status == 403);

    auto tunnel = proxy.connect();
    auto authority = "127.0.0.1:" + std::to_string(origin.port());
    send(tunnel, "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n\r\n");
    auto connect_resp = read_response(tunnel);
    REQUIRE(connect_resp);
    CHECK(connect_resp->status == 403);

    CHECK(hits == 0);
    CHECK(proxy.ctx->stats.blocked_requests == 2);
}

TEST_CASE("blocking private addresses refuses destinations resolving to loopback", "[proxy]") {
    std::atomic<int> hits{0};
    FakeOrigin origin(FakeOrigin::http(
        [](const proxy::http::Request&, tcp::socket& s) { send(s, http_response(200, "ok")); }, &hits));
    proxy::Config config;
    config.access.block_private = true;
    config.connect_ports = {origin.port()};
    ProxyHarness proxy(config);

    for (auto host : {std::string("127.0.0.1"), std::string("localhost")}) {
        INFO(host);
        auto client = proxy.connect();
        auto authority = host + ":" + std::to_string(origin.port());
        send(client, "GET http://" + authority + "/ HTTP/1.1\r\nHost: " + authority + "\r\n\r\n");
        auto resp = read_response(client);
        REQUIRE(resp);
        CHECK(resp->status == 403);
    }

    auto tunnel = proxy.connect();
    auto authority = "127.0.0.1:" + std::to_string(origin.port());
    send(tunnel, "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n\r\n");
    auto connect_resp = read_response(tunnel);
    REQUIRE(connect_resp);
    CHECK(connect_resp->status == 403);
    CHECK(hits == 0);
}

TEST_CASE("CONNECT tunnels bytes both ways", "[proxy]") {
    FakeOrigin echo_server(echo);
    proxy::Config config;
    config.connect_ports = {echo_server.port()};
    ProxyHarness proxy(config);

    auto client = proxy.connect();
    auto authority = "127.0.0.1:" + std::to_string(echo_server.port());
    send(client, "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n\r\n");
    CHECK(read_head(client).rfind("HTTP/1.1 200", 0) == 0);

    send(client, "ping through the tunnel");
    std::array<char, 4096> buffer{};
    std::string echoed;
    while (echoed.size() < 23) {
        boost::system::error_code ec;
        auto n = read_some(client, buffer, ec);
        REQUIRE_FALSE(ec);
        echoed.append(buffer.data(), n);
    }
    CHECK(echoed == "ping through the tunnel");
}

TEST_CASE("CONNECT pipelined after a GET keeps the bytes that follow it", "[proxy]") {
    FakeOrigin origin(FakeOrigin::http([](const proxy::http::Request&, tcp::socket& s) {
        std::this_thread::sleep_for(50ms);
        send(s, http_response(200, "first", "Cache-Control: no-store\r\n"));
    }));
    FakeOrigin echo_server(echo);
    proxy::Config config;
    config.connect_ports = {echo_server.port()};
    ProxyHarness proxy(config);

    // The tunnel payload is sent optimistically together with both requests, so
    // the session still has a read outstanding when the tunnel takes over the
    // socket and these bytes sit in its buffer: none of them may be lost.
    auto client = proxy.connect();
    auto authority = "127.0.0.1:" + std::to_string(echo_server.port());
    send(client, get(origin.url("/first")) + "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority +
                     "\r\n\r\nearly-bytes");
    std::string leftover;
    auto resp = read_response(client, leftover);
    REQUIRE(resp);
    CHECK(resp->body == "first");

    std::array<char, 4096> buffer{};
    while (leftover.find("\r\n\r\n") == std::string::npos) {
        boost::system::error_code ec;
        auto n = read_some(client, buffer, ec);
        REQUIRE_FALSE(ec);
        leftover.append(buffer.data(), n);
    }
    CHECK(leftover.rfind("HTTP/1.1 200", 0) == 0);
    auto echoed = leftover.substr(leftover.find("\r\n\r\n") + 4);
    while (echoed.size() < 11) {
        boost::system::error_code ec;
        auto n = read_some(client, buffer, ec);
        REQUIRE_FALSE(ec);
        echoed.append(buffer.data(), n);
    }
    CHECK(echoed == "early-bytes");

    // The tunnel keeps working afterwards.
    send(client, "more");
    echoed.clear();
    while (echoed.size() < 4) {
        boost::system::error_code ec;
        auto n = read_some(client, buffer, ec);
        REQUIRE_FALSE(ec);
        echoed.append(buffer.data(), n);
    }
    CHECK(echoed == "more");
}

TEST_CASE("shutdown lets an in-flight response finish", "[proxy]") {
    std::promise<void> got_request;
    FakeOrigin origin(FakeOrigin::http([&](const proxy::http::Request&, tcp::socket& s) {
        got_request.set_value();
        std::this_thread::sleep_for(200ms);
        send(s, http_response(200, "finished", "Cache-Control: no-store\r\n"));
    }));
    ProxyHarness proxy;

    auto client = proxy.connect();
    send(client, get(origin.url("/in-flight")));
    REQUIRE(got_request.get_future().wait_for(kIoTimeout) == std::future_status::ready);
    auto stopping = std::async(std::launch::async, [&] { proxy.stop(); });

    auto resp = read_response(client);
    REQUIRE(resp);
    CHECK(resp->status == 200);
    CHECK(resp->body == "finished");
    CHECK(header(*resp, "Connection") == "close");
    CHECK(stopping.wait_for(kIoTimeout) == std::future_status::ready);
}

// Streaming and upstream connection reuse

namespace {

    std::string pattern(std::size_t size) {
        std::string out(size, '\0');
        for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<char>((i * 7 + i / 4096) % 251);
        return out;
    }

    // Writes `body` with chunked framing, in pieces of `piece` bytes.
    void send_chunked(tcp::socket& s, const std::string& head, const std::string& body, std::size_t piece) {
        send(s, head);
        for (std::size_t at = 0; at < body.size(); at += piece) {
            auto part = body.substr(at, piece);
            std::ostringstream size;
            size << std::hex << part.size();
            send(s, size.str() + "\r\n" + part + "\r\n");
        }
        send(s, "0\r\n\r\n");
    }

}  // namespace

TEST_CASE("responses larger than the body limit stream through without being cached", "[proxy][stream]") {
    const auto body = pattern(20u << 20);  // 20 MiB, over the 8 MiB --max-body / --max-object
    std::atomic<int> hits{0};
    FakeOrigin origin(FakeOrigin::http(
        [&](const proxy::http::Request&, tcp::socket& s) {
            send(s, http_response(200, body, "Cache-Control: max-age=60\r\n"));
        },
        &hits));
    ProxyHarness proxy;

    for (int i = 0; i < 2; ++i) {
        auto client = proxy.connect();
        send(client, get(origin.url("/big")));
        auto resp = read_response(client);
        REQUIRE(resp);
        CHECK(resp->status == 200);
        CHECK(header(*resp, "Content-Length") == std::to_string(body.size()));
        CHECK(header(*resp, "X-Cache") == "MISS");
        CHECK(resp->body.size() == body.size());
        CHECK(resp->body == body);
    }
    CHECK(hits == 2);
}

TEST_CASE("chunked and close-delimited bodies are re-framed for the client", "[proxy][stream]") {
    const auto body = pattern(3u << 20);
    FakeOrigin chunked_origin(FakeOrigin::http([&](const proxy::http::Request&, tcp::socket& s) {
        send_chunked(s, "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nTransfer-Encoding: chunked\r\n\r\n",
                     body, 64 * 1024);
    }));
    FakeOrigin close_origin(FakeOrigin::http([&](const proxy::http::Request&, tcp::socket& s) {
        send(s, "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body);
        s.close();
    }));
    ProxyHarness proxy;

    SECTION("HTTP/1.1 clients get chunked framing") {
        for (auto* origin : {&chunked_origin, &close_origin}) {
            auto client = proxy.connect();
            send(client, get(origin->url("/stream")));
            auto resp = read_response(client);
            REQUIRE(resp);
            CHECK(header(*resp, "Transfer-Encoding") == "chunked");
            CHECK_FALSE(resp->headers.contains("Content-Length"));
            CHECK(resp->body == body);
        }
    }
    SECTION("HTTP/1.0 clients get the body delimited by connection close") {
        auto client = proxy.connect();
        auto url = chunked_origin.url("/stream");
        send(client, "GET " + url + " HTTP/1.0\r\n\r\n");
        auto resp = read_response(client);
        REQUIRE(resp);
        CHECK_FALSE(resp->headers.contains("Transfer-Encoding"));
        CHECK(header(*resp, "Connection") == "close");
        CHECK(resp->body == body);
    }
}

TEST_CASE("a streamed response that fits --max-object is cached", "[proxy][stream]") {
    const auto body = pattern(300 * 1024);
    std::atomic<int> hits{0};
    FakeOrigin origin(FakeOrigin::http(
        [&](const proxy::http::Request&, tcp::socket& s) {
            send_chunked(s,
                         "HTTP/1.1 200 OK\r\nCache-Control: max-age=60\r\nTransfer-Encoding: chunked\r\n\r\n",
                         body, 16 * 1024);
        },
        &hits));
    ProxyHarness proxy;

    for (const char* expected : {"MISS", "HIT"}) {
        auto client = proxy.connect();
        send(client, get(origin.url("/cacheable")));
        auto resp = read_response(client);
        REQUIRE(resp);
        CHECK(header(*resp, "X-Cache") == expected);
        CHECK(resp->body == body);
    }
    CHECK(hits == 1);
}

TEST_CASE("a streamed response that outgrows --max-object is not cached", "[proxy][stream]") {
    const auto body = pattern(300 * 1024);
    std::atomic<int> hits{0};
    FakeOrigin origin(FakeOrigin::http(
        [&](const proxy::http::Request&, tcp::socket& s) {
            // No Content-Length: the proxy only finds out it is too large while copying.
            send_chunked(s,
                         "HTTP/1.1 200 OK\r\nCache-Control: max-age=60\r\nTransfer-Encoding: chunked\r\n\r\n",
                         body, 16 * 1024);
        },
        &hits));
    proxy::Config config;
    config.cache.max_entry_bytes = 100 * 1024;
    ProxyHarness proxy(config);

    for (int i = 0; i < 2; ++i) {
        auto client = proxy.connect();
        send(client, get(origin.url("/too-big")));
        auto resp = read_response(client);
        REQUIRE(resp);
        CHECK(header(*resp, "X-Cache") == "MISS");
        CHECK(resp->body == body);
    }
    CHECK(hits == 2);
    CHECK(proxy.ctx->cache.stats().entries == 0);
}

TEST_CASE("a slow client slows the origin down instead of filling memory", "[proxy][stream]") {
    constexpr std::size_t kTotal = 64u << 20;
    constexpr std::size_t kBlock = 64 * 1024;
    std::atomic<std::size_t> sent{0};
    FakeOrigin origin(FakeOrigin::http([&](const proxy::http::Request&, tcp::socket& s) {
        send(s, "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nContent-Length: " + std::to_string(kTotal) +
                    "\r\nConnection: close\r\n\r\n");
        const std::string block(kBlock, 'x');
        boost::system::error_code ec;
        while (sent < kTotal && !ec) {
            asio::write(s, asio::buffer(block), ec);
            if (!ec) sent += kBlock;
        }
    }));
    ProxyHarness proxy;

    auto client = proxy.connect();
    send(client, get(origin.url("/firehose")));
    std::this_thread::sleep_for(500ms);  // do not read yet
    // Only kernel socket buffers (a few MiB per socket) and one 16 KiB piece in
    // the proxy can be in flight; without backpressure the origin would be done.
    CHECK(sent < 32u << 20);

    std::string leftover;
    auto resp = read_response(client, leftover);
    REQUIRE(resp);
    CHECK(resp->body.size() == kTotal);
    CHECK(sent == kTotal);
}

TEST_CASE("a client leaving mid-stream stops the origin transfer", "[proxy][stream]") {
    std::promise<bool> origin_write_failed;
    FakeOrigin origin(FakeOrigin::http([&](const proxy::http::Request&, tcp::socket& s) {
        send(s, "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nContent-Length: 1073741824\r\n\r\n");
        const std::string block(64 * 1024, 'x');
        boost::system::error_code ec;
        auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!ec && std::chrono::steady_clock::now() < deadline) asio::write(s, asio::buffer(block), ec);
        origin_write_failed.set_value(static_cast<bool>(ec));
    }));
    ProxyHarness proxy;

    {
        auto client = proxy.connect();
        send(client, get(origin.url("/endless")));
        std::array<char, 4096> buffer{};
        boost::system::error_code ec;
        read_some(client, buffer, ec);  // the response has started
        REQUIRE_FALSE(ec);
    }  // client goes away

    auto failed = origin_write_failed.get_future();
    REQUIRE(failed.wait_for(kIoTimeout) == std::future_status::ready);
    CHECK(failed.get());
    CHECK(proxy.ctx->stats.client_aborts == 1);
}

TEST_CASE("shutdown cuts a never-ending stream after the grace period", "[proxy][stream]") {
    std::promise<void> streaming;
    std::atomic<bool> stop_origin{false};
    FakeOrigin origin(FakeOrigin::http([&](const proxy::http::Request&, tcp::socket& s) {
        send(s, "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nTransfer-Encoding: chunked\r\n\r\n");
        streaming.set_value();
        boost::system::error_code ec;
        while (!ec && !stop_origin) {
            asio::write(s, asio::buffer(std::string("1\r\nx\r\n")), ec);
            std::this_thread::sleep_for(50ms);
        }
    }));
    proxy::Config config;
    config.upstream_timeout = std::chrono::seconds{1};
    ProxyHarness proxy(config);

    auto client = proxy.connect();
    send(client, get(origin.url("/forever")));
    REQUIRE(streaming.get_future().wait_for(kIoTimeout) == std::future_status::ready);

    auto started = std::chrono::steady_clock::now();
    auto stopping = std::async(std::launch::async, [&] { proxy.stop(); });
    // Keep reading so only the grace period, not a blocked write, ends the stream.
    std::array<char, 4096> buffer{};
    boost::system::error_code ec;
    while (!ec) read_some(client, buffer, ec);
    CHECK(stopping.wait_for(kIoTimeout) == std::future_status::ready);
    CHECK(std::chrono::steady_clock::now() - started < 4s);
    stop_origin = true;
}

TEST_CASE("sequential requests reuse one pooled origin connection", "[proxy][pool]") {
    std::atomic<int> connections{0}, requests{0};
    FakeOrigin origin(FakeOrigin::keep_alive(
        [](const proxy::http::Request& req, tcp::socket& s) {
            send(s, http_response(200, req.target, "Cache-Control: no-store\r\n", true));
        },
        &connections, &requests));
    ProxyHarness proxy;

    for (const char* path : {"/one", "/two", "/three"}) {
        auto client = proxy.connect();
        send(client, get(origin.url(path)));
        auto resp = read_response(client);
        REQUIRE(resp);
        CHECK(resp->body == path);
    }
    CHECK(requests == 3);
    CHECK(connections == 1);
    CHECK(proxy.ctx->pool.reused() == 2);
}

TEST_CASE("a pooled connection closed by the origin is retried on a fresh one", "[proxy][pool]") {
    std::atomic<int> connections{0};
    FakeOrigin origin([&](tcp::socket& s) {
        int index = ++connections;
        auto first = read_request(s);
        if (!first) return;
        send(s, http_response(200, "first-" + std::to_string(index), "Cache-Control: no-store\r\n", true));
        if (index == 1) {
            // Keep-alive timeout races with the next request: read it, then
            // close without answering.
            read_request(s);
            s.close();
        }
    });
    ProxyHarness proxy;

    auto client = proxy.connect();
    send(client, get(origin.url("/a")));
    std::string leftover;
    auto first = read_response(client, leftover);
    REQUIRE(first);
    CHECK(first->body == "first-1");

    send(client, get(origin.url("/b")));
    auto second = read_response(client, leftover);
    REQUIRE(second);
    CHECK(second->status == 200);
    CHECK(second->body == "first-2");
    CHECK(connections == 2);
}

TEST_CASE("non-idempotent requests never use a pooled connection", "[proxy][pool]") {
    std::atomic<int> connections{0}, requests{0};
    FakeOrigin origin(FakeOrigin::keep_alive(
        [](const proxy::http::Request& req, tcp::socket& s) {
            send(s, http_response(200, req.method, "Cache-Control: no-store\r\n", true));
        },
        &connections, &requests));
    ProxyHarness proxy;
    auto authority = "127.0.0.1:" + std::to_string(origin.port());

    auto client = proxy.connect();
    send(client, get(origin.url("/x")));
    std::string leftover;
    REQUIRE(read_response(client, leftover));
    send(client,
         "POST " + origin.url("/x") + " HTTP/1.1\r\nHost: " + authority + "\r\nContent-Length: 1\r\n\r\nz");
    auto resp = read_response(client, leftover);
    REQUIRE(resp);
    CHECK(resp->body == "POST");
    CHECK(connections == 2);
}

TEST_CASE("a disabled pool sends Connection: close and opens a connection per request", "[proxy][pool]") {
    std::atomic<int> connections{0}, requests{0};
    std::atomic<int> close_headers{0};
    FakeOrigin origin(FakeOrigin::keep_alive(
        [&](const proxy::http::Request& req, tcp::socket& s) {
            if (req.headers.has_token("Connection", "close")) ++close_headers;
            send(s, http_response(200, "ok", "Cache-Control: no-store\r\n", true));
        },
        &connections, &requests));
    proxy::Config config;
    config.pool_size = 0;
    ProxyHarness proxy(config);

    for (int i = 0; i < 2; ++i) {
        auto client = proxy.connect();
        send(client, get(origin.url("/")));
        REQUIRE(read_response(client));
    }
    CHECK(connections == 2);
    CHECK(close_headers == 2);
}