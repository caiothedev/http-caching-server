#include <catch2/catch_test_macros.hpp>

#include <string>

#include "http/parser.hpp"

using namespace proxy::http;

namespace {

    // Feeds the message one byte at a time to exercise every state transition
    // across buffer boundaries.
    template <typename Parser>
    std::size_t feed_bytewise(Parser& p, std::string_view data) {
        std::size_t used = 0;
        for (char c : data) {
            if (p.status() != ParseStatus::NeedMore) break;
            used += p.feed(std::string_view(&c, 1));
        }
        return used;
    }

}  // namespace

TEST_CASE("parses a simple absolute-form GET", "[parser]") {
    RequestParser p;
    std::string raw = "GET http://example.com/index.html HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\n\r\n";
    CHECK(p.feed(raw) == raw.size());
    REQUIRE(p.status() == ParseStatus::Complete);
    auto req = p.take();
    CHECK(req.method == "GET");
    CHECK(req.target == "http://example.com/index.html");
    CHECK(req.version == Version{1, 1});
    CHECK(req.headers.get("host") == "example.com");
    CHECK(req.headers.get("ACCEPT") == "*/*");
    CHECK(req.body.empty());
}

TEST_CASE("parses requests split at arbitrary byte boundaries", "[parser]") {
    RequestParser p;
    std::string raw =
        "POST http://example.com/form HTTP/1.1\r\nHost: example.com\r\nContent-Length: 11\r\n\r\nhello world";
    CHECK(feed_bytewise(p, raw) == raw.size());
    REQUIRE(p.status() == ParseStatus::Complete);
    CHECK(p.take().body == "hello world");
}

TEST_CASE("rejects bare LF line endings", "[parser]") {
    RequestParser p;
    p.feed("GET / HTTP/1.0\nHost: a\n\n");
    CHECK(p.error() == ParseError::BadHeader);
}

TEST_CASE("accepts leading empty CRLF lines", "[parser]") {
    RequestParser p;
    p.feed("\r\nGET / HTTP/1.0\r\nHost: a\r\n\r\n");
    REQUIRE(p.status() == ParseStatus::Complete);
    CHECK(p.take().version == Version{1, 0});
}

TEST_CASE("decodes chunked bodies and ignores extensions and trailers", "[parser]") {
    RequestParser p;
    std::string raw =
        "POST /upload HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5;ext=1\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\nX-Trailer: yes\r\n\r\n";
    CHECK(feed_bytewise(p, raw) == raw.size());
    REQUIRE(p.status() == ParseStatus::Complete);
    CHECK(p.was_chunked());
    CHECK(p.take().body == "hello world");
}

TEST_CASE("stops at the end of a message so pipelined requests can be parsed", "[parser]") {
    RequestParser p;
    std::string first = "GET http://a/1 HTTP/1.1\r\nHost: a\r\n\r\n";
    std::string second = "GET http://a/2 HTTP/1.1\r\nHost: a\r\n\r\n";
    std::string both = first + second;

    auto used = p.feed(both);
    CHECK(used == first.size());
    REQUIRE(p.status() == ParseStatus::Complete);
    CHECK(p.take().target == "http://a/1");

    p.reset();
    p.feed(std::string_view(both).substr(used));
    REQUIRE(p.status() == ParseStatus::Complete);
    CHECK(p.take().target == "http://a/2");
}

TEST_CASE("exposes headers before the body is complete (Expect: 100-continue)", "[parser]") {
    RequestParser p;
    p.feed("POST /x HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n");
    CHECK(p.status() == ParseStatus::NeedMore);
    CHECK(p.headers_complete());
    CHECK(p.headers().has_token("Expect", "100-continue"));
    p.feed("data");
    CHECK(p.status() == ParseStatus::Complete);
}

TEST_CASE("rejects malformed requests", "[parser]") {
    auto parse = [](const std::string& raw) {
        RequestParser p;
        p.feed(raw);
        return p;
    };

    CHECK(parse("GET /\r\n\r\n").error() == ParseError::BadStartLine);
    CHECK(parse("GET  / HTTP/1.1\r\nHost: a\r\n\r\n").error() == ParseError::BadStartLine);
    CHECK(parse("GET / HTTP/2.0\r\nHost: a\r\n\r\n").error() == ParseError::BadVersion);
    CHECK(parse("GET / HTTP/1.1\r\n\r\n").error() == ParseError::BadHeader);  // missing Host
    CHECK(parse("GET / HTTP/1.1\r\nHost : a\r\n\r\n").error() == ParseError::BadHeader);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n").error() == ParseError::BadHeader);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: abc\r\n\r\n").error() ==
          ParseError::BadContentLength);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1, 2\r\n\r\n").error() ==
          ParseError::BadContentLength);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n").error() ==
          ParseError::UnsupportedTransferEncoding);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n").error() ==
          ParseError::BadChunk);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n").error() ==
          ParseError::UnsupportedTransferEncoding);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n").error() ==
          ParseError::BadTransferEncoding);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue, other\r\nContent-Length: 1\r\n\r\n").error() ==
          ParseError::ExpectationFailed);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nExpect: something-else\r\n\r\n").error() ==
          ParseError::ExpectationFailed);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nX-Test: value\001bad\r\n\r\n").error() ==
          ParseError::BadHeader);
}

TEST_CASE("accepts repeated identical Content-Length values", "[parser]") {
    RequestParser p;
    p.feed("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2, 2\r\n\r\nok");
    REQUIRE(p.status() == ParseStatus::Complete);
    CHECK(p.take().body == "ok");
}

TEST_CASE("rejects Transfer-Encoding combined with Content-Length", "[parser]") {
    RequestParser p;
    p.feed("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n");
    CHECK(p.error() == ParseError::BadTransferEncoding);
}

TEST_CASE("enforces header and body limits", "[parser]") {
    ParserLimits limits;
    limits.max_header_bytes = 64;
    limits.max_body_bytes = 4;

    RequestParser big_headers(limits);
    big_headers.feed("GET / HTTP/1.1\r\nHost: a\r\nX-Long: " + std::string(100, 'x') + "\r\n\r\n");
    CHECK(big_headers.error() == ParseError::HeadersTooLarge);

    RequestParser big_body(limits);
    big_body.feed("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n");
    CHECK(big_body.error() == ParseError::BodyTooLarge);

    RequestParser big_chunks(limits);
    big_chunks.feed("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n3\r\n");
    CHECK(big_chunks.error() == ParseError::BodyTooLarge);
}

TEST_CASE("parses responses with Content-Length", "[parser][response]") {
    ResponseParser p("GET");
    p.feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello");
    REQUIRE(p.status() == ParseStatus::Complete);
    auto resp = p.take();
    CHECK(resp.status == 200);
    CHECK(resp.reason == "OK");
    CHECK(resp.body == "hello");
}

TEST_CASE("reads response bodies until close when unframed", "[parser][response]") {
    ResponseParser p("GET");
    p.feed("HTTP/1.0 200 OK\r\n\r\npart one, ");
    p.feed("part two");
    CHECK(p.status() == ParseStatus::NeedMore);
    p.finish();
    REQUIRE(p.status() == ParseStatus::Complete);
    CHECK(p.take().body == "part one, part two");
}

TEST_CASE("responses to HEAD and 204/205/304 have no body", "[parser][response]") {
    ResponseParser head("HEAD");
    head.feed("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n");
    CHECK(head.status() == ParseStatus::Complete);

    ResponseParser not_modified("GET");
    not_modified.feed("HTTP/1.1 304 Not Modified\r\nETag: \"x\"\r\n\r\n");
    CHECK(not_modified.status() == ParseStatus::Complete);

    ResponseParser no_content("GET");
    no_content.feed("HTTP/1.1 204 No Content\r\n\r\n");
    CHECK(no_content.status() == ParseStatus::Complete);

    ResponseParser reset_content("GET");
    reset_content.feed("HTTP/1.1 205 Reset Content\r\nContent-Length: 0\r\n\r\n");
    CHECK(reset_content.status() == ParseStatus::Complete);

    ResponseParser invalid_reset_content("GET");
    invalid_reset_content.feed("HTTP/1.1 205 Reset Content\r\nContent-Length: 4\r\n\r\ndata");
    CHECK(invalid_reset_content.error() == ParseError::BadContentLength);

    ResponseParser encoded_reset_content("GET");
    encoded_reset_content.feed("HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked\r\n\r\n");
    CHECK(encoded_reset_content.error() == ParseError::BadTransferEncoding);
}

EST_CASE("rejects unsupported transfer codings in origin responses", "[parser][response]") {
    ResponseParser p("GET");
    p.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n");
    CHECK(p.error() == ParseError::UnsupportedTransferEncoding);
}

TEST_CASE("parses 101 as a terminal response for explicit rejection by proxy", "[parser][response]") {
    ResponseParser p("GET");
    p.feed("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    REQUIRE(p.status() == ParseStatus::Complete);
    CHECK(p.take().status == 101);
}

TEST_CASE("skips interim 1xx responses", "[parser][response]") {
    ResponseParser p("POST");
    p.feed("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok");
    REQUIRE(p.status() == ParseStatus::Complete);
    auto resp = p.take();
    CHECK(resp.status == 201);
    CHECK(resp.body == "ok");
}

TEST_CASE("accepts status lines without a reason phrase", "[parser][response]") {
    ResponseParser p("GET");
    p.feed("HTTP/1.1 404\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(p.status() == ParseStatus::Complete);
    auto resp = p.take();
    CHECK(resp.status == 404);
    CHECK(resp.reason.empty());
}

TEST_CASE("reports truncated responses", "[parser][response]") {
    ResponseParser p("GET");
    p.feed("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort");
    p.finish();
    CHECK(p.status() == ParseStatus::Error);
    CHECK(p.error() == ParseError::UnexpectedEof);

    ResponseParser bad("GET");
    bad.feed("HTTP/1.1 2x0 OK\r\n\r\n");
    CHECK(bad.error() == ParseError::BadStartLine);
}

TEST_CASE("includes the Expectation Failed reason phrase", "[message]") {
    CHECK(reason_phrase(417) == "Expectation Failed");
}

TEST_CASE("serializes messages", "[message]") {
    Response resp;
    resp.status = 404;
    resp.headers.add("Content-Length", "3");
    resp.body = "nop";
    CHECK(serialize(resp) == "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nnop");

    Request req;
    req.method = "GET";
    req.target = "/";
    req.headers.add("Host", "a");
    CHECK(serialize(req) == "GET / HTTP/1.1\r\nHost: a\r\n\r\n");
}

TEST_CASE("removes hop-by-hop headers including those named in Connection", "[message]") {
    Headers h;
    h.add("Connection", "close, X-Secret");
    h.add("X-Secret", "1");
    h.add("Keep-Alive", "timeout=5");
    h.add("Transfer-Encoding", "chunked");
    h.add("Content-Type", "text/html");
    remove_hop_by_hop(h);
    CHECK(h.size() == 1);
    CHECK(h.contains("Content-Type"));
}

TEST_CASE("keep-alive defaults depend on the HTTP version", "[message]") {
    Request r11;
    r11.version = {1, 1};
    CHECK(wants_keep_alive(r11));
    r11.headers.add("Connection", "close");
    CHECK_FALSE(wants_keep_alive(r11));

    Request r10;
    r10.version = {1, 0};
    CHECK_FALSE(wants_keep_alive(r10));
    r10.headers.add("Proxy-Connection", "Keep-Alive");
    CHECK(wants_keep_alive(r10));
}