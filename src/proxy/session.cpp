#include "proxy/session.hpp"

#include <algorithm>
#include <sstream>

#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

#include "cache/cache_control.hpp"
#include "proxy/tunnel.hpp"
#include "util/logger.hpp"

namespace proxy {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    namespace {

        constexpr std::string_view kViaToken = "http-caching-proxy";
        const std::string kVia = "1.1 " + std::string(kViaToken);
        const std::string kContinue = "HTTP/1.1 100 Continue\r\n\r\n";
        const std::string kConnectEstablished = "HTTP/1.1 200 Connection Established\r\n\r\n";

        bool is_unsafe_method(std::string_view m) {
            return m != "GET" && m != "HEAD" && m != "OPTIONS" && m != "TRACE";
        }

        void append_via(http::Headers& headers) {
            auto via = headers.get_combined("Via");
            headers.set("Via", via.empty() ? kVia : via + ", " + kVia);
        }

    }  // namespace

    Session::Session(tcp::socket socket, std::shared_ptr<ProxyContext> ctx)
        : client_(std::move(socket)),
        ctx_(std::move(ctx)),
        deadline_(client_.get_executor()),
        id_(ctx_->next_session_id.fetch_add(1)),
        parser_(ctx_->config.limits) {
        boost::system::error_code ec;
        auto ep = client_.remote_endpoint(ec);
        peer_ = ec ? "?" : ep.address().to_string() + ":" + std::to_string(ep.port());
        ctx_->stats.connections++;
        ctx_->stats.active_connections++;
    }

    Session::~Session() { ctx_->stats.active_connections--; }

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
            if (self->upstream_ || !self->write_buffer_.empty()) return;
            self->close();
        });
    }

    // ---------------------------------------------------------------------------
    // Reading and parsing
    // ---------------------------------------------------------------------------

    void Session::do_read() {
        if (closed_) return;
        client_.async_read_some(asio::buffer(buffer_),
                                [self = shared_from_this()](const boost::system::error_code& ec, std::size_t n) {
                                    self->on_read(ec, n);
                                });
    }

    void Session::on_read(const boost::system::error_code& ec, std::size_t n) {
        if (closed_) return;
        if (ec) {
            if (ec != asio::error::eof && ec != asio::error::operation_aborted) {
                LOG_DEBUG("[" << id_ << "] read error: " << ec.message());
            }
            return close();
        }
        deadline_.extend(ctx_->config.client_idle_timeout);
        pending_.append(buffer_.data(), n);
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
            asio::async_write(client_, asio::buffer(kContinue),
                            [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
                                if (ec) return self->close();
                                self->do_read();
                            });
            return;
        }
        do_read();
    }

    // ---------------------------------------------------------------------------
    // Request dispatch
    // ---------------------------------------------------------------------------

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

        // A request that already went through us means the proxy is pointed at itself.
        if (req.headers.get_combined("Via").find(kViaToken) != std::string::npos) {
            current_ = std::move(req);
            return send_error(508, "request loop detected");
        }

        cache_key_ = url_->to_string();
        cache::EntryPtr stale;

        if (ctx_->config.cache_enabled && req.method == "GET" && !cache::is_conditional(req)) {
            auto result = ctx_->cache.lookup(cache_key_, req, cache::Clock::now());
            if (result.status == cache::LookupStatus::Fresh) {
                current_ = std::move(req);
                return serve_entry(result.entry, "HIT");
            }
            auto cc = cache::parse_cache_control(req.headers.get_combined("Cache-Control"));
            if (cc.only_if_cached) {
                current_ = std::move(req);
                return send_error(504, "resource not in cache (only-if-cached)");
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
            << "  \"requests\": " << s.requests.load() << ",\n"
            << "  \"tunnels\": " << s.tunnels.load() << ",\n"
            << "  \"upstream_errors\": " << s.upstream_errors.load() << ",\n"
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

    // ---------------------------------------------------------------------------
    // Forwarding and caching
    // ---------------------------------------------------------------------------

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
        up.headers.set("Connection", "close");
        if (has_body) up.headers.set("Content-Length", std::to_string(req.body.size()));
        up.body = std::move(req.body);
        req.body.clear();
        current_ = std::move(req);

        LOG_DEBUG("[" << id_ << "] -> " << url_->host << ":" << url_->port << " " << up.method << " "
                    << up.target << (stale ? " (revalidating)" : ""));

        request_time_ = cache::Clock::now();
        auto self = shared_from_this();
        upstream_ = std::make_shared<UpstreamExchange>(
            client_.get_executor(), url_->host, url_->port, http::serialize(up), up.method,
            ctx_->config.upstream_timeout, ctx_->config.limits,
            [self, stale](UpstreamResult result) { self->on_upstream(std::move(result), stale); });
        upstream_->start();
    }

    void Session::on_upstream(UpstreamResult result, const cache::EntryPtr& stale) {
        upstream_.reset();
        if (closed_) return;

        if (!result.ok) {
            ctx_->stats.upstream_errors++;
            LOG_WARN("[" << id_ << "] upstream error: " << result.error);
            return send_error(result.timed_out ? 504 : 502, result.error);
        }

        auto response_time = cache::Clock::now();
        http::Response& resp = result.response;
        auto& cache = ctx_->cache;

        if (resp.status == 101) {
            LOG_WARN("[" << id_ << "] upstream attempted unsupported protocol upgrade");
            keep_alive_ = false;
            return send_error(502, "upstream protocol upgrade is not supported");
        }

        if (stale && resp.status == 304) {
            auto entry = cache.refresh(cache_key_, stale, current_, resp, request_time_, response_time);
            return serve_entry(entry, "REVALIDATED");
        }

        if (ctx_->config.cache_enabled) {
            if (current_.method == "GET") {
                if (cache.store(cache_key_, current_, resp, request_time_, response_time)) {
                    LOG_DEBUG("[" << id_ << "] stored " << cache_key_);
                } else if (stale) {
                    cache.invalidate(cache_key_);  // the new response replaced the stored one
                }
            } else if (is_unsafe_method(current_.method) && resp.status < 400) {
                // RFC 9111 §4.4: a successful unsafe request invalidates the target
                // URI and the URIs in Location / Content-Location (same origin only).
                cache.invalidate(cache_key_);
                for (auto name : {"Location", "Content-Location"}) {
                    if (auto value = resp.headers.get(name)) {
                        if (auto ref = http::resolve_reference(*url_, *value)) cache.invalidate(ref->to_string());
                    }
                }
            }
        }
        send_response(std::move(resp), ctx_->config.cache_enabled && current_.method == "GET" ? "MISS" : "");
    }

    void Session::serve_entry(const cache::EntryPtr& entry, std::string_view cache_status) {
        http::Response resp = entry->response;
        resp.headers.set("Age", std::to_string(entry->current_age(cache::Clock::now()).count()));
        send_response(std::move(resp), cache_status);
    }

    // ---------------------------------------------------------------------------
    // Writing responses
    // ---------------------------------------------------------------------------

    void Session::send_response(http::Response resp, std::string_view cache_status) {
        if (closed_) return;
        http::remove_hop_by_hop(resp.headers);
        resp.version = {1, 1};

        // The body was fully buffered (and de-chunked), so frame it with Content-Length.
        if (!http::response_has_no_body(current_.method, resp.status)) {
            resp.headers.set("Content-Length", std::to_string(resp.body.size()));
        } else {
            if (resp.status == 205) {
                resp.headers.set("Content-Length", "0");
            } else if (current_.method != "HEAD") {
                resp.headers.remove("Content-Length");
            }
            resp.body.clear();
        }
        append_via(resp.headers);
        if (!cache_status.empty()) resp.headers.set("X-Cache", std::string(cache_status));
        resp.headers.set("Connection", keep_alive_ ? "keep-alive" : "close");

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_);
        LOG_INFO("[" << id_ << "] " << peer_ << " \"" << current_.method << " "
                    << (url_ ? url_->to_string() : current_.target) << "\" " << resp.status << " "
                    << resp.body.size() << "B " << (cache_status.empty() ? "-" : cache_status) << " "
                    << elapsed.count() << "ms");

        write_buffer_ = http::serialize(resp);
        deadline_.extend(ctx_->config.client_idle_timeout);
        asio::async_write(client_, asio::buffer(write_buffer_),
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
        if (closed_) return;
        write_buffer_.clear();
        if (ec) {
            LOG_DEBUG("[" << id_ << "] write error: " << ec.message());
            return close();
        }
        if (!keep_alive_ || shutting_down_) return close();
        deadline_.extend(ctx_->config.client_idle_timeout);
        process();  // there may already be a pipelined request in `pending_`
    }

    // ---------------------------------------------------------------------------
    // CONNECT tunnelling
    // ---------------------------------------------------------------------------

    void Session::handle_connect(const http::Request& req) {
        keep_alive_ = false;
        auto target = http::parse_authority(req.target, 0);
        if (!target) return send_error(400, "CONNECT target must be host:port");

        const auto& allowed = ctx_->config.connect_ports;
        if (!allowed.empty() && std::find(allowed.begin(), allowed.end(), target->port) == allowed.end()) {
            return send_error(403, "CONNECT to port " + std::to_string(target->port) + " is not allowed");
        }

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
                asio::async_connect(
                    *self->tunnel_socket_, results,
                    [self, host](const boost::system::error_code& e, const tcp::endpoint&) {
                        if (self->closed_) return;
                        if (e) {
                            self->ctx_->stats.upstream_errors++;
                            return self->send_error(502, "cannot connect to " + host + ": " + e.message());
                        }
                        asio::async_write(self->client_, asio::buffer(kConnectEstablished),
                                        [self](const boost::system::error_code& we, std::size_t) {
                                            if (we || self->closed_) return self->close();
                                            self->start_tunnel();
                                        });
                    });
            });
    }

    void Session::start_tunnel() {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_);
        LOG_INFO("[" << id_ << "] " << peer_ << " \"CONNECT " << current_.target << "\" 200 tunnel "
                    << elapsed.count() << "ms");

        // From here on the Tunnel owns both sockets; this session is finished.
        closed_ = true;
        deadline_.stop();
        ctx_->stats.tunnels++;
        auto tunnel = std::make_shared<Tunnel>(std::move(client_), std::move(*tunnel_socket_),
                                            ctx_->config.tunnel_idle_timeout, id_);
        if (tunnel_handler_) tunnel_handler_(tunnel);
        tunnel->start(std::move(pending_));
    }

    void Session::close() {
        if (closed_) return;
        closed_ = true;
        deadline_.stop();
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