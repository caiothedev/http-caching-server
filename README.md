# HTTP CACHING PROXY SERVER - ᴇɴ

An HTTP proxy server with a caching system, developed in C++ as a study project, focusing on network programming, TCP socket communication, concurrency, and the handling of HTTP requests.

## Objectives

- Develop an HTTP caching proxy server in C++ capable of processing GET, POST, and CONNECT requests.
- Utilize TCP socket programming based on the Boost library for sending and receiving packets, and handle simultaneous requests from various sources through multithreading.
- Gain an understanding of the fundamentals of the HTTP/1.1 and TCP/IP protocols.

## Stack

- **C++17**
- **Boost.Asio**: asynchronous TCP I/O, with one `io_context` run by a thread pool and one *strand* per connection
- **CMake** (≥ 3.16)
- **Catch2 v3**: unit tests, fetched automatically by CMake

## Features

- **GET / POST / PUT / DELETE / ...**: absolute-form requests (`GET http://host/path HTTP/1.1`) are forwarded to the origin in origin form
- **CONNECT**: bidirectional TCP tunnel (HTTPS) with half-close and an idle timeout; allowed ports are configurable (default 443)
- **Streamed responses with backpressure**: the body is relayed to the client while it arrives, one piece per client write, so responses of any size pass through in bounded memory and the first bytes reach the client immediately; `chunked` and close-delimited bodies are re-chunked for HTTP/1.1 clients
- **Keep-alive pool to origins**: idle upstream connections are reused (`--pool-size`, `--pool-idle-timeout`); only idempotent requests use a pooled connection, and are retried once on a fresh connection if the origin closed it meanwhile
- **Zero-copy cache hits**: cached bodies are written straight from the cache entry with scatter/gather I/O
- **Hand-written incremental HTTP/1.1 parser**: `Content-Length`, chunked encoding, read-until-close bodies, interim 1xx responses, pipelining, strict CRLF/header validation, size limits
- Transfer coding support is intentionally limited to a single `chunked` coding; unsupported or ambiguous `Transfer-Encoding` combinations are rejected
- `Expect` supports only `100-continue`; other expectations receive `417 Expectation Failed`
- HTTP protocol upgrades (`101 Switching Protocols` / `Upgrade`) are not supported and are rejected explicitly
- `205 Reset Content` responses are validated as bodyless and framed with `Content-Length: 0`
- **In-memory cache (RFC 9111)**:
  - Byte-bounded LRU with a per-object size limit
  - `Cache-Control` (`max-age`, `s-maxage`, `no-store`, `no-cache`, `private`, `public`, `must-revalidate`, `max-stale`, `min-fresh`, `only-if-cached`), `Expires`, `Pragma: no-cache`
  - Age calculation and heuristic freshness from `Last-Modified`
  - Revalidation with `If-None-Match` / `If-Modified-Since` and `304 Not Modified` merging
  - `Vary` support; invalidation after successful POST/PUT/DELETE
  - `X-Cache: HIT | MISS | REVALIDATED` response header
- Client keep-alive, `Expect: 100-continue`, hop-by-hop header removal, `Via`, loop detection
- Bounded simultaneous client connections (including CONNECT tunnels) and per-message body size, with a 128 MiB aggregate in-flight body budget; clients over the limit get `503 Service Unavailable` with `Retry-After`
- Destination access policy: host allowlist (`--allow-hosts`, with `*.example.com` wildcards) and optional refusal of loopback/private/link-local addresses after DNS resolution (`--block-private`), for both forwarding and CONNECT
- A client that disconnects while the origin is still answering cancels the upstream exchange
- Accept errors such as `EMFILE` are retried with exponential backoff instead of spinning
- Separate absolute-duration and inactivity deadlines for upstream exchanges, plus idle-client and tunnel timeouts
- Graceful SIGINT/SIGTERM shutdown: stop accepting new clients, finish in-flight HTTP responses, and close active CONNECT tunnels
- Statistics endpoint: `GET /__proxy/stats` (JSON)

## Build

Dependencies (Ubuntu/Debian):

```bash
sudo apt install build-essential cmake libboost-dev
```

Build and run the tests:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
scripts/smoke_test.sh build/http_proxy   # end-to-end test
```

AddressSanitizer/UBSan build (recommended while developing):

```bash
cmake -S . -B build-asan -DPROXY_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
```

## Usage

```bash
./build/http_proxy --port 8080 --threads 4 --cache-size 128
```

```bash
# first request: X-Cache: MISS; second: X-Cache: HIT
curl -x http://127.0.0.1:8080 -i http://example.com/

# HTTPS goes through a CONNECT tunnel (not cached)
curl -x http://127.0.0.1:8080 https://example.com/

# statistics
curl http://127.0.0.1:8080/__proxy/stats
```

Run `./build/http_proxy --help` for all options. The proxy listens on `127.0.0.1` by default. Be careful with `--listen 0.0.0.0`: anyone on the network could use an open proxy. When exposing it, combine `--allow-hosts` and/or `--block-private` with firewall rules.

## Architecture

```
src/
├── main.cpp              # CLI, io_context + thread pool, signals
├── util/                 # strings, thread-safe logger
├── http/                 # messages, incremental parser, URLs, HTTP dates
├── cache/                # LruCache<K,V>, Cache-Control, RFC 9111 policy (HttpCache)
└── proxy/
    ├── server            # acceptor; each connection gets its own strand
    ├── session           # lifecycle of one client connection
    ├── upstream          # one request/response exchange with the origin
    ├── tunnel            # CONNECT byte relay
    └── deadline.hpp      # steady_timer based timeout watchdog
tests/                    # Catch2 unit tests and in-process proxy integration tests
```

Request flow: `Session` reads and parses → looks up `HttpCache` → *hit*: answer directly; *stale*: send a conditional request; *miss*: `UpstreamExchange` takes a pooled connection or connects to the origin → the headers and the first body bytes are sent to the client right away and the rest is relayed piece by piece (a copy is kept for the cache while it fits `--max-object`) → the response is stored if policy allows.

`HttpCache` is shared across threads behind a mutex. All other state belongs to a single session, whose handlers run serialized on its strand.

## Documentation

- [How to build, run and test](docs/README.md): prerequisites, CMake and CLI options, usage examples, error codes and troubleshooting
- [Architecture](docs/ARCHITECTURE.en.md): layers, concurrency model, request lifecycle, parser, caching policy and timeouts
- [Contributing](CONTRIBUTING.md): code standards, tests, commits and pull requests

[CI](.github/workflows/ci.yml) builds with GCC and Clang (Release, and Debug with ASan/UBSan), then runs the unit tests and the smoke test on every push to `main` and every pull request.

by caiothedev<br />
#http #server #c++ #proxy #cache