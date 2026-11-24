#include "proxy/session.hpp"

#include <algorithm>
#include <array>
#include <sstream>

#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

#include "cache/cache_control.hpp"
#include "proxy/tunnel.hpp"
#include "util/logger.hpp"
#include "util/strings.hpp"

namespace proxy {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    namespace {

        constexpr std::string_view kViaToken = "http-caching-proxy";
        const std::string kVia = "1.1 " + std::string(kViaToken);
        const std::string kContinue = "HTTP/1.1 100 Continue\r\n\r\n";
        const std::string kConnectEstablished = "HTTP/1.1 200 Connection Established\r\n\r\n";

        // While a request is in progress the client socket is still read, to
        // notice disconnects; pipelined bytes are kept, up to this many.
        constexpr std::size_t kMaxBufferedWhileBusy = 64 * 1024;

        bool is_unsafe_method(std::string_view m) {
            return m != "GET" && m != "HEAD" && m != "OPTIONS" && m != "TRACE";
        }

        // RFC 9110 §9.2.2: safe to repeat, so may be sent on a reused connection.
        bool is_idempotent(std::string_view m) { return !is_unsafe_method(m) || m == "PUT" || m == "DELETE"; }

        void append_via(http::Headers& headers) {
            auto via = headers.get_combined("Via");
            headers.set("Via", via.empty() ? kVia : via + ", " + kVia);
        }

    }  // namespace

    Session::Session(tcp::socket socket, std::shared_ptr<ProxyContext> ctx)
        : client_(std::move(socket)),
          ctx_(std::move(ctx)),
          deadline_(client_.get_executor()),
          shutdown_grace_(client_.get_executor()),
          id_(ctx_->next_session_id.fetch_add(1)),
          parser_(ctx_->config.limits) {
        boost::system::error_code ec;
        auto ep = client_.remote_endpoint(ec);
        peer_ = ec ? "?" : ep.address().to_string() + ":" + std::to_string(ep.port());
        ctx_->stats.connections++;
    }

    Session::~Session() {
        if (!connection_slot_transferred_) ctx_->stats.active_connections--;
    }

    void Session::start() {
        LOG_DEBUG("[" << id_ << "] connection from " << peer_);
        auto self = shared_from_this();
        deadline_.start(ctx_->config.client_idle_timeout, [self] {
            LOG_DEBUG("[" << self->id_ << "] timeout, closing");
            self->close();
        });
        do_read();
    }

    void Session::shutdown() {
        auto self = shared_from_this();
        asio::post(client_.get_executor(), [self] {
            if (self->closed_ || self->shutting_down_) return;
            self->shutting_down_ = true;
            self->keep_alive_ = false;
            // Let an upstream exchange or response write finish; otherwise this
            // connection is idle/partial and can be closed immediately.
            if (self->upstream_ || self->writing_) {
                if (self->streaming_) self->arm_shutdown_grace();
                return;
            }
            self->close();
        });
    }

    // A streamed body can go on for a long time: once shutdown was requested it
    // gets --timeout to finish, then it is cut so shutdown stays bounded.
    // (Waiting for response headers is already bounded by the exchange.)
    void Session::arm_shutdown_grace() {
        if (grace_armed_) return;
        grace_armed_ = true;
        auto self = shared_from_this();
        shutdown_grace_.start(ctx_->config.upstream_timeout, [self] {
            LOG_INFO("[" << self->id_ << "] shutdown grace period over, closing streamed response");
            self->close();
        });
    }

    // Reading and parsing

    void Session::do_read() {
        if (closed_ || reading_) return;
        reading_ = true;
        client_.async_read_some(asio::buffer(buffer_),
                                [self = shared_from_this()](const boost::system::error_code& ec, std::size_t n) {
                                    self->on_read(ec, n);
                                });
    }

    void Session::on_read(const boost::system::error_code& ec, std::size_t n) {
        reading_ = false;
        if (closed_) return;
        pending_.append(buffer_.data(), n);

        if (tunnel_pending_) {
            // start_tunnel() cancelled this read so the Tunnel can own the socket.
            if (ec && ec != asio::error::operation_aborted) return close();
            return start_tunnel();
        }
        if (ec) {
            if (upstream_) {
                // Like Squid's default (half_closed_clients off), end-of-input
                // while waiting for the origin is treated as the client leaving.
                ctx_->stats.client_aborts++;
                LOG_DEBUG("[" << id_ << "] client went away, cancelling upstream request");
            } else if (ec != asio::error::eof && ec != asio::error::operation_aborted) {
                LOG_DEBUG("[" << id_ << "] read error: " << ec.message());
            }
            return close();
        }
        if (busy()) {
            // Keep watching for a disconnect; process() picks the buffered bytes
            // up once the current response has been written.
            if (pending_.size() < kMaxBufferedWhileBusy) do_read();
            return;
        }
        deadline_.extend(ctx_->config.client_idle_timeout);
        process();
    }

    void Session::process() {
        auto used = parser_.feed(pending_);
        pending_.erase(0, used);

        switch (parser_.status()) {
            case http::ParseStatus::Error: {
                int status = 400;
                switch (parser_.error()) {
                    case http::ParseError::HeadersTooLarge: status = 431; break;
                    case http::ParseError::BodyTooLarge: status = 413; break;
                    case http::ParseError::BadVersion: status = 505; break;
                    case http::ParseError::UnsupportedTransferEncoding: status = 501; break;
                    case http::ParseError::ExpectationFailed: status = 417; break;
                    default: break;
                }
                LOG_WARN("[" << id_ << "] bad request from " << peer_ << ": " << parser_.error_message());
                keep_alive_ = false;
                current_ = {};
                url_.reset();
                started_ = std::chrono::steady_clock::now();
                return send_error(status, parser_.error_message());
            }
            case http::ParseStatus::Complete: {
                auto req = parser_.take();
                parser_.reset();
                continue_sent_ = false;
                return handle_request(std::move(req));
            }
            case http::ParseStatus::NeedMore:
                break;
        }

        // Clients such as curl send "Expect: 100-continue" before large bodies and
        // wait for permission. We answer it ourselves and strip it upstream.
        if (parser_.headers_complete() && !continue_sent_ &&
            parser_.headers().has_token("Expect", "100-continue")) {
            continue_sent_ = true;
            writing_ = true;
            asio::async_write(client_, asio::buffer(kContinue),
                              [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
                                  self->writing_ = false;
                                  if (ec) return self->close();
                                  self->process();  // body bytes may have arrived during the write
                              });
            return;
        }
        do_read();
    }

    // Request dispatch

    void Session::handle_request(http::Request req) {
        ctx_->stats.requests++;
        started_ = std::chrono::steady_clock::now();
        keep_alive_ = http::wants_keep_alive(req);
        url_.reset();
        deadline_.suspend();  // the upstream exchange has its own timeout

        if (req.method == "CONNECT") {
            current_ = std::move(req);
            return handle_connect(current_);
        }
        if (req.headers.contains("Upgrade") || req.headers.has_token("Connection", "upgrade")) {
            current_ = std::move(req);
            keep_alive_ = false;
            return send_error(501, "HTTP protocol upgrades are not supported");
        }

        url_ = http::parse_absolute_url(req.target);
        if (!url_) {
            current_ = std::move(req);
            if (!current_.target.empty() && current_.target.front() == '/') return handle_local(current_);
            return send_error(400, "invalid request target");
        }
        if (url_->scheme != "http") {
            current_ = std::move(req);
            return send_error(400, "only http:// URLs can be proxied; use CONNECT for https");
        }
        if (!ctx_->config.access.host_allowed(url_->host)) {
            current_ = std::move(req);
            ctx_->stats.blocked_requests++;
            LOG_WARN("[" << id_ << "] destination host not allowed: " << url_->host);
            return send_error(403, "destination host is not allowed");
        }

        // A request that already went through us means the proxy is pointed at itself.
        if (req.headers.get_combined("Via").find(kViaToken) != std::string::npos) {
            current_ = std::move(req);
            return send_error(508, "request loop detected");
        }

        cache_key_ = url_->to_string();
        cache::EntryPtr stale;
        const auto request_cc = cache::parse_cache_control(req.headers.get_combined("Cache-Control"));
        const bool cache_lookup_eligible = req.method == "GET" && !cache::is_conditional(req);

        // only-if-cached is an origin-access prohibition, not merely a cache
        // lookup hint. Enforce it before any path that can forward upstream.
        if (request_cc.only_if_cached && (!ctx_->config.cache_enabled || !cache_lookup_eligible)) {
            current_ = std::move(req);
            return send_error(504, "resource not available in cache (only-if-cached)");
        }

        if (ctx_->config.cache_enabled && cache_lookup_eligible) {
            auto result = ctx_->cache.lookup(cache_key_, req, cache::Clock::now());
            if (result.status == cache::LookupStatus::Fresh) {
                current_ = std::move(req);
                return serve_entry(result.entry, "HIT");
            }
            if (request_cc.only_if_cached) {
                current_ = std::move(req);
                return send_error(504, "resource not available in cache (only-if-cached)");
            }
            if (result.status == cache::LookupStatus::NeedsValidation) stale = result.entry;
        }

        forward(std::move(req), std::move(stale));
    }

    void Session::handle_local(const http::Request& req) {
        if (req.method != "GET" || req.target != "/__proxy/stats") {
            return send_error(400, "this is a proxy server: send absolute-form requests (http://host/path) "
                                "or GET /__proxy/stats");
        }
        auto c = ctx_->cache.stats();
        auto& s = ctx_->stats;
        std::ostringstream json;
        json << "{\n"
             << "  \"connections\": " << s.connections.load() << ",\n"
             << "  \"active_connections\": " << s.active_connections.load() << ",\n"
             << "  \"active_tunnels\": " << s.active_tunnels.load() << ",\n"
             << "  \"requests\": " << s.requests.load() << ",\n"
             << "  \"tunnels\": " << s.tunnels.load() << ",\n"
             << "  \"upstream_errors\": " << s.upstream_errors.load() << ",\n"
             << "  \"client_aborts\": " << s.client_aborts.load() << ",\n"
             << "  \"blocked_requests\": " << s.blocked_requests.load() << ",\n"
             << "  \"rejected_connections\": " << s.rejected_connections.load() << ",\n"
             << "  \"upstream_pool\": {\n"
             << "    \"idle\": " << ctx_->pool.idle() << ",\n"
             << "    \"reused\": " << ctx_->pool.reused() << "\n"
             << "  },\n"
             << "  \"cache\": {\n"
             << "    \"enabled\": " << (ctx_->config.cache_enabled ? "true" : "false") << ",\n"
             << "    \"entries\": " << c.entries << ",\n"
             << "    \"bytes\": " << c.bytes << ",\n"
             << "    \"capacity\": " << c.capacity << ",\n"
             << "    \"hits\": " << c.hits << ",\n"
             << "    \"misses\": " << c.misses << ",\n"
             << "    \"revalidations\": " << c.revalidations << ",\n"
             << "    \"stores\": " << c.stores << ",\n"
             << "    \"evictions\": " << c.evictions << ",\n"
             << "    \"invalidations\": " << c.invalidations << "\n"
             << "  }\n"
             << "}\n";

        http::Response resp;
        resp.status = 200;
        resp.headers.add("Content-Type", "application/json");
        resp.headers.add("Cache-Control", "no-store");
        resp.body = json.str();
        send_response(std::move(resp));
    }

    // Forwarding and caching

    void Session::forward(http::Request req, cache::EntryPtr stale) {
        http::Request up;
        up.method = req.method;
        up.target = url_->path_and_query;  // absolute-form -> origin-form
        up.version = {1, 1};
        up.headers = req.headers;

        bool has_body = up.headers.contains("Content-Length") || up.headers.contains("Transfer-Encoding") ||
                        !req.body.empty();
        http::remove_hop_by_hop(up.headers);
        up.headers.remove("Expect");
        up.headers.set("Host", url_->authority());

        if (stale) {
            const auto& stored = stale->response.headers;
            if (auto etag = stored.get("ETag")) up.headers.set("If-None-Match", *etag);
            if (auto lm = stored.get("Last-Modified")) up.headers.set("If-Modified-Since", *lm);
        }
        append_via(up.headers);
        // HTTP/1.1 connections are persistent by default; without a pool the
        // origin is told the connection ends with this exchange.
        if (!ctx_->pool.enabled()) up.headers.set("Connection", "close");
        if (has_body) up.headers.set("Content-Length", std::to_string(req.body.size()));
        up.body = std::move(req.body);
        req.body.clear();
        current_ = std::move(req);
        stale_ = std::move(stale);

        LOG_DEBUG("[" << id_ << "] -> " << url_->host << ":" << url_->port << " " << up.method << " "
                      << up.target << (stale_ ? " (revalidating)" : ""));

        UpstreamOptions options;
        options.timeout = ctx_->config.upstream_timeout;
        options.limits = ctx_->config.limits;
        options.block_private = ctx_->config.access.block_private;
        options.pool = &ctx_->pool;
        options.reuse_allowed = is_idempotent(up.method);

        UpstreamExchange::Handlers handlers;
        auto self = shared_from_this();
        handlers.on_head = [self](http::Response resp, bool complete) {
            self->on_upstream_head(std::move(resp), complete);
        };
        handlers.on_body = [self](std::string data, bool last) {
            self->on_upstream_body(std::move(data), last);
        };
        handlers.on_error = [self](UpstreamError error) { self->on_upstream_error(std::move(error)); };

        request_time_ = cache::Clock::now();
        upstream_ =
            std::make_shared<UpstreamExchange>(client_.get_executor(), url_->host, url_->port,
                                               http::serialize(up), up.method, options, std::move(handlers));
        upstream_->start();
        // Watch the client while the origin works, so a disconnect cancels the exchange.
        if (pending_.size() < kMaxBufferedWhileBusy) do_read();
    }

    void Session::on_upstream_error(UpstreamError error) {
        upstream_.reset();
        if (closed_) return;

        if (error.forbidden) {
            ctx_->stats.blocked_requests++;
            LOG_WARN("[" << id_ << "] " << error.message);
            return send_error(403, "destination address is not allowed");
        }
        ctx_->stats.upstream_errors++;
        if (streaming_) {
            // The status line is already out: all we can do is cut the
            // connection, so the client sees a truncated body (a missing
            // chunked terminator or a short Content-Length body).
            LOG_WARN("[" << id_ << "] upstream failed mid-response after " << stream_bytes_
                         << " body bytes: " << error.message);
            if (stale_) ctx_->cache.invalidate(cache_key_);
            return close();
        }
        LOG_WARN("[" << id_ << "] upstream error: " << error.message);
        send_error(error.timed_out ? 504 : 502, error.message);
    }

    void Session::invalidate_after_unsafe(const http::Response& resp) {
        // RFC 9111 §4.4: a successful unsafe request invalidates the target
        // URI and the URIs in Location / Content-Location (same origin only).
        auto& cache = ctx_->cache;
        cache.invalidate(cache_key_);
        for (auto name : {"Location", "Content-Location"}) {
            if (auto value = resp.headers.get(name)) {
                if (auto ref = http::resolve_reference(*url_, *value)) cache.invalidate(ref->to_string());
            }
        }
    }

    void Session::on_upstream_head(http::Response resp, bool complete) {
        if (complete) upstream_.reset();
        if (closed_) return;
        response_time_ = cache::Clock::now();

        if (resp.status == 101) {
            LOG_WARN("[" << id_ << "] upstream attempted unsupported protocol upgrade");
            upstream_.reset();
            keep_alive_ = false;
            return send_error(502, "upstream protocol upgrade is not supported");
        }
        if (complete) return on_upstream_complete(std::move(resp));
        start_stream(std::move(resp));
    }

    // The whole response arrived at once: it is cached and sent as one write.
    void Session::on_upstream_complete(http::Response resp) {
        auto& cache = ctx_->cache;
        auto stale = std::move(stale_);

        if (stale && resp.status == 304) {
            auto entry = cache.refresh(cache_key_, stale, current_, resp, request_time_, response_time_);
            return serve_entry(entry, "REVALIDATED");
        }

        if (ctx_->config.cache_enabled) {
            if (current_.method == "GET") {
                if (auto entry = cache.store(cache_key_, current_, resp, request_time_, response_time_)) {
                    LOG_DEBUG("[" << id_ << "] stored " << cache_key_);
                } else if (stale) {
                    cache.invalidate(cache_key_);  // the new response replaced the stored one
                }
            } else if (is_unsafe_method(current_.method) && resp.status < 400) {
                invalidate_after_unsafe(resp);
            }
        }
        send_response(std::move(resp), cache_status_for_miss());
    }

    std::string_view Session::cache_status_for_miss() const {
        return ctx_->config.cache_enabled && current_.method == "GET" ? "MISS" : "";
    }

    // The body is still arriving: send the headers now and relay the body as
    // the exchange hands it over, one piece per client write.
    void Session::start_stream(http::Response resp) {
        auto& cache = ctx_->cache;
        const auto& cache_opts = cache.options();

        // Framing as sent by the origin (before hop-by-hop removal drops it).
        std::optional<std::uint64_t> length;
        if (!resp.headers.contains("Transfer-Encoding")) {
            std::uint64_t n = 0;
            if (auto cl = resp.headers.get("Content-Length"); cl && util::parse_uint(util::trim(*cl), n))
                length = n;
        }

        // Cache: keep a copy while it streams if the response may be stored and
        // can still fit; give up as soon as it grows past --max-object.
        stream_cacheable_ = ctx_->config.cache_enabled && current_.method == "GET" &&
                            cache::is_storable(current_, resp) &&
                            (!length || *length <= cache_opts.max_entry_bytes);
        if (stale_ && !stream_cacheable_) cache.invalidate(cache_key_);
        if (ctx_->config.cache_enabled && is_unsafe_method(current_.method) && resp.status < 400) {
            invalidate_after_unsafe(resp);
        }

        std::string first = std::move(resp.body);
        resp.body.clear();
        if (stream_cacheable_) {
            stream_store_ = resp;
            stream_store_.body = first;
        }

        // Client framing: keep the origin's length when known; otherwise
        // chunked for HTTP/1.1 clients, or end-of-connection for HTTP/1.0.
        stream_chunked_ = false;
        if (!length) {
            if (current_.version.major == 1 && current_.version.minor >= 1) stream_chunked_ = true;
            else keep_alive_ = false;
        }
        prepare_head(resp, cache_status_for_miss());
        resp.headers.remove("Content-Length");
        if (length) resp.headers.set("Content-Length", std::to_string(*length));
        if (stream_chunked_) resp.headers.set("Transfer-Encoding", "chunked");

        streaming_ = true;
        stream_done_ = false;
        if (shutting_down_) arm_shutdown_grace();  // shutdown arrived while waiting for the headers
        stream_status_ = resp.status;
        stream_bytes_ = first.size();
        stream_cache_status_ = std::string(cache_status_for_miss());
        write_stream(http::serialize_head(resp), std::move(first), false);
    }

    void Session::on_upstream_body(std::string data, bool last) {
        if (last) upstream_.reset();  // the exchange is finished (connection pooled or closed)
        if (closed_) return;
        stream_bytes_ += data.size();

        if (stream_cacheable_) {
            if (stream_store_.body.size() + data.size() > ctx_->cache.options().max_entry_bytes) {
                stream_cacheable_ = false;  // too large to cache: stop copying
                stream_store_ = {};
                if (stale_) ctx_->cache.invalidate(cache_key_);
            } else {
                stream_store_.body += data;
            }
        }
        // Store as soon as the origin finished, before the client can see the
        // end of the response and send its next request.
        if (last) store_streamed_response();
        write_stream({}, std::move(data), last);
    }

    void Session::store_streamed_response() {
        if (stream_cacheable_) {
            if (ctx_->cache.store(cache_key_, current_, stream_store_, request_time_, response_time_)) {
                LOG_DEBUG("[" << id_ << "] stored " << cache_key_ << " (streamed)");
            } else if (stale_) {
                ctx_->cache.invalidate(cache_key_);
            }
        }
        stream_cacheable_ = false;
        stream_store_ = {};
        stale_.reset();
    }

    void Session::write_stream(std::string head, std::string data, bool last) {
        stream_done_ = last;
        write_head_ = std::move(head);
        stream_data_ = std::move(data);
        if (stream_chunked_) {
            if (!stream_data_.empty()) {
                std::ostringstream size;
                size << std::hex << stream_data_.size();
                write_head_ += size.str() + "\r\n";
            }
            stream_suffix_ = stream_data_.empty() ? "" : "\r\n";
            if (last) stream_suffix_ += "0\r\n\r\n";
        } else {
            stream_suffix_.clear();
        }

        std::array<asio::const_buffer, 3> buffers{asio::buffer(write_head_), asio::buffer(stream_data_),
                                                  asio::buffer(stream_suffix_)};
        deadline_.extend(ctx_->config.client_idle_timeout);
        writing_ = true;
        asio::async_write(client_, buffers,
                          [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
                              self->on_stream_write(ec);
                          });
    }

    void Session::on_stream_write(const boost::system::error_code& ec) {
        writing_ = false;
        if (closed_) return;
        if (ec) {
            // The client went away mid-response: detected here when the write
            // fails before the watching read sees end-of-input.
            if (upstream_) ctx_->stats.client_aborts++;
            LOG_DEBUG("[" << id_ << "] write error mid-stream: " << ec.message());
            return close();
        }
        if (!stream_done_) {
            deadline_.suspend();  // the exchange's inactivity deadline covers the wait
            if (upstream_) upstream_->resume();
            return;
        }

        // Response complete.
        streaming_ = false;
        write_head_.clear();
        stream_data_.clear();
        log_access(stream_status_, stream_bytes_, stream_cache_status_);
        after_response();
    }

    void Session::serve_entry(const cache::EntryPtr& entry, std::string_view cache_status) {
        // Only the header section is copied; the body is written straight from
        // the cache entry, which stays alive (shared_ptr) until the write ends.
        http::Response head;
        head.status = entry->response.status;
        head.reason = entry->response.reason;
        head.headers = entry->response.headers;
        head.headers.set("Age", std::to_string(entry->current_age(cache::Clock::now()).count()));
        serving_ = entry;
        write_response(std::move(head), entry->response.body, cache_status);
    }

    // Writing responses

    void Session::prepare_head(http::Response& resp, std::string_view cache_status) {
        http::remove_hop_by_hop(resp.headers);
        resp.version = {1, 1};
        append_via(resp.headers);
        if (!cache_status.empty()) resp.headers.set("X-Cache", std::string(cache_status));
        resp.headers.set("Connection", keep_alive_ ? "keep-alive" : "close");
    }

    void Session::log_access(int status, std::uint64_t body_bytes, std::string_view cache_status) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_);
        LOG_INFO("[" << id_ << "] " << peer_ << " \"" << current_.method << " "
                     << (url_ ? url_->to_string() : current_.target) << "\" " << status << " " << body_bytes
                     << "B " << (cache_status.empty() ? "-" : cache_status) << " " << elapsed.count()
                     << "ms");
    }

    void Session::send_response(http::Response resp, std::string_view cache_status) {
        if (closed_) return;
        write_resp_ = std::move(resp);
        std::string_view body = write_resp_.body;
        http::Response head;
        head.status = write_resp_.status;
        head.reason = std::move(write_resp_.reason);
        head.headers = std::move(write_resp_.headers);
        write_response(std::move(head), body, cache_status);
    }

    // Writes `head` + `body` without copying the body. `body` must stay valid
    // until the write completes (it lives in write_resp_ or serving_).
    void Session::write_response(http::Response head, std::string_view body, std::string_view cache_status) {
        if (closed_) return;
        prepare_head(head, cache_status);

        // The body is complete in memory, so frame it with Content-Length.
        if (!http::response_has_no_body(current_.method, head.status)) {
            head.headers.set("Content-Length", std::to_string(body.size()));
        } else {
            if (head.status == 205) {
                head.headers.set("Content-Length", "0");
            } else if (current_.method != "HEAD") {
                head.headers.remove("Content-Length");
            }
            body = {};
        }
        log_access(head.status, body.size(), cache_status);

        write_head_ = http::serialize_head(head);
        std::array<asio::const_buffer, 2> buffers{asio::buffer(write_head_),
                                                  asio::buffer(body.data(), body.size())};
        deadline_.extend(ctx_->config.client_idle_timeout);
        writing_ = true;
        asio::async_write(client_, buffers,
                          [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
                              self->on_write(ec);
                          });
    }

    void Session::send_error(int status, const std::string& message) {
        http::Response resp;
        resp.status = status;
        resp.headers.add("Content-Type", "text/plain; charset=utf-8");
        resp.headers.add("Cache-Control", "no-store");
        resp.body = std::to_string(status) + " " + std::string(http::reason_phrase(status)) + ": " + message + "\n";
        send_response(std::move(resp));
    }

    void Session::on_write(const boost::system::error_code& ec) {
        writing_ = false;
        if (closed_) return;
        write_head_.clear();
        write_resp_ = {};
        serving_.reset();
        if (ec) {
            LOG_DEBUG("[" << id_ << "] write error: " << ec.message());
            return close();
        }
        after_response();
    }

    void Session::after_response() {
        if (!keep_alive_ || shutting_down_) return close();
        deadline_.extend(ctx_->config.client_idle_timeout);
        process();  // there may already be a pipelined request in `pending_`
    }

    // CONNECT tunnelling

    void Session::handle_connect(const http::Request& req) {
        keep_alive_ = false;
        auto target = http::parse_authority(req.target, 0);
        if (!target) return send_error(400, "CONNECT target must be host:port");

        const auto& allowed = ctx_->config.connect_ports;
        if (!allowed.empty() && std::find(allowed.begin(), allowed.end(), target->port) == allowed.end()) {
            return send_error(403, "CONNECT to port " + std::to_string(target->port) + " is not allowed");
        }
        if (!ctx_->config.access.host_allowed(target->host)) {
            ctx_->stats.blocked_requests++;
            LOG_WARN("[" << id_ << "] CONNECT destination host not allowed: " << target->host);
            return send_error(403, "destination host is not allowed");
        }
        connecting_ = true;

        LOG_DEBUG("[" << id_ << "] CONNECT " << target->to_string());
        resolver_ = std::make_unique<tcp::resolver>(client_.get_executor());
        tunnel_socket_ = std::make_unique<tcp::socket>(client_.get_executor());
        deadline_.extend(ctx_->config.upstream_timeout);

        auto self = shared_from_this();
        auto host = target->to_string();
        resolver_->async_resolve(
            target->host, std::to_string(target->port),
            [self, host](const boost::system::error_code& ec, const tcp::resolver::results_type& results) {
                if (self->closed_) return;
                if (ec) {
                    self->ctx_->stats.upstream_errors++;
                    return self->send_error(502, "cannot resolve " + host + ": " + ec.message());
                }
                self->connect_endpoints_ = self->ctx_->config.access.filter(results);
                if (self->connect_endpoints_.empty()) {
                    self->ctx_->stats.blocked_requests++;
                    LOG_WARN("[" << self->id_ << "] CONNECT " << host << " resolves to a non-public address");
                    return self->send_error(403, "destination address is not allowed");
                }
                asio::async_connect(
                    *self->tunnel_socket_, self->connect_endpoints_,
                    [self, host](const boost::system::error_code& e, const tcp::endpoint&) {
                        if (self->closed_) return;
                        if (e) {
                            self->ctx_->stats.upstream_errors++;
                            return self->send_error(502, "cannot connect to " + host + ": " + e.message());
                        }
                        self->writing_ = true;
                        asio::async_write(self->client_, asio::buffer(kConnectEstablished),
                                          [self](const boost::system::error_code& we, std::size_t) {
                                              self->writing_ = false;
                                              if (we || self->closed_) return self->close();
                                              self->start_tunnel();
                                          });
                    });
            });
    }

    void Session::start_tunnel() {
        if (reading_) {
            // A read is still outstanding (possible after pipelined requests).
            // The Tunnel must not inherit it, or the bytes it returns would be
            // lost: cancel it and continue from on_read() with its data.
            tunnel_pending_ = true;
            boost::system::error_code ignored;
            client_.cancel(ignored);
            return;
        }
        tunnel_pending_ = false;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_);
        LOG_INFO("[" << id_ << "] " << peer_ << " \"CONNECT " << current_.target << "\" 200 tunnel "
                    << elapsed.count() << "ms");

        // From here on the Tunnel owns both sockets; this session is finished.
        closed_ = true;
        deadline_.stop();
        connection_slot_transferred_ = true;
        ctx_->stats.tunnels++;
        ctx_->stats.active_tunnels++;
        auto ctx = ctx_;
        auto tunnel = std::make_shared<Tunnel>(std::move(client_), std::move(*tunnel_socket_),
                                            ctx_->config.tunnel_idle_timeout, id_, [ctx] {
                                                ctx->stats.active_tunnels--;
                                                ctx->stats.active_connections--;
                                            });
        if (tunnel_handler_) tunnel_handler_(tunnel);
        tunnel->start(std::move(pending_));
    }

    void Session::close() {
        if (closed_) return;
        closed_ = true;
        deadline_.stop();
        shutdown_grace_.stop();
        if (upstream_) {
            upstream_->cancel();
            upstream_.reset();
        }
        boost::system::error_code ignored;
        if (resolver_) resolver_->cancel();
        if (tunnel_socket_) tunnel_socket_->close(ignored);
        client_.shutdown(tcp::socket::shutdown_both, ignored);
        client_.close(ignored);
    }

}  // namespace proxy