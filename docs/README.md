# DOCUMENTATION - ᴇɴ

How to build, run and test the proxy.

- [Architecture](ARCHITECTURE.en.md)
- [Contributing](../CONTRIBUTING.md)
- [Versão em português](README.pt-br.md)

## Contents

1. [Prerequisites](#1-prerequisites)
2. [Building](#2-building)
3. [Running](#3-running)
4. [Command-line options](#4-command-line-options)
5. [Using the proxy](#5-using-the-proxy)
6. [Tests](#6-tests)
7. [Continuous integration](#7-continuous-integration)
8. [Troubleshooting](#8-troubleshooting)

## 1. Prerequisites

| Tool | Minimum version | Notes |
| --- | --- | --- |
| C++17 compiler | GCC 9 / Clang 10 | tested with GCC 13 and Clang 18 |
| CMake | 3.16 | |
| Boost | 1.70 | headers only (Boost.Asio); nothing to link |
| Git + internet access | — | CMake downloads Catch2 v3 when tests are enabled |
| Ninja | optional | faster builds (`-G Ninja`) |

Installing the dependencies:

```bash
# Ubuntu / Debian
sudo apt install build-essential cmake ninja-build libboost-dev

# Fedora
sudo dnf install gcc-c++ cmake ninja-build boost-devel

# Arch Linux
sudo pacman -S base-devel cmake ninja boost

# macOS (Homebrew)
brew install cmake ninja boost
```

## 2. Building

Release build with tests:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces two executables in `build/`:

- `http_proxy`: the server;
- `proxy_tests`: the unit tests.

### CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Debug` | `Debug`, `Release`, `RelWithDebInfo` |
| `PROXY_BUILD_TESTS` | `ON` | builds `proxy_tests` (downloads Catch2) |
| `PROXY_ENABLE_SANITIZERS` | `OFF` | builds with AddressSanitizer + UndefinedBehaviorSanitizer |
| `CMAKE_COMPILE_WARNING_AS_ERROR` | `OFF` | turns warnings into errors (used by CI) |

Build without tests (no internet needed):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPROXY_BUILD_TESTS=OFF
cmake --build build -j
```

Sanitizer build, recommended while developing:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPROXY_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
```

> With Clang on Linux, sanitizers need `compiler-rt` (`sudo apt install libclang-rt-dev` on Ubuntu).

## 3. Running

```bash
./build/http_proxy
```

By default the proxy listens on `127.0.0.1:8080`, uses one thread per CPU and a 64 MiB cache. Expected output:

```
2026-01-01 12:00:00.000 [INFO ] [t1234] proxy listening on 127.0.0.1:8080 with 8 threads, cache 64 MiB
```

Each request produces one log line:

```
[session-id] client "METHOD URL" status bytes X-Cache duration
[2] 127.0.0.1:52006 "GET http://example.com/" 200 1256B HIT 0ms
```

`Ctrl+C` (SIGINT) or SIGTERM stops the server, which then prints the final cache statistics.

## 4. Command-line options

| Option | Default | Description |
| --- | --- | --- |
| `-l, --listen ADDR` | `127.0.0.1` | address to bind (IPv4 or IPv6) |
| `-p, --port N` | `8080` | port to listen on |
| `-t, --threads N` | CPU count | pool threads running the `io_context` |
| `--cache-size MB` | `64` | total cache size in MiB |
| `--max-object MB` | `8` | largest cacheable response in MiB |
| `--no-cache` | — | disables the cache (pure forwarding proxy) |
| `--timeout SEC` | `30` | upstream connect/read timeout |
| `--idle-timeout SEC` | `60` | idle keep-alive client timeout |
| `--tunnel-timeout SEC` | `300` | idle CONNECT tunnel timeout |
| `--connect-ports LIST` | `443` | comma-separated ports allowed for CONNECT, or `any` |
| `--log-level LEVEL` | `info` | `debug`, `info`, `warn` or `error` |
| `-h, --help` | — | shows the help |

Options accept both `--port 8080` and `--port=8080`.

Example:

```bash
./build/http_proxy --port 3128 --threads 4 --cache-size 256 --max-object 16 --log-level debug
```

> **Security:** `--listen 0.0.0.0` exposes the proxy to the whole network. There is no authentication: anyone who can reach the port can use it as an open proxy. Only do this on trusted networks.

## 5. Using the proxy

### With curl

```bash
# First request: X-Cache: MISS
curl -x http://127.0.0.1:8080 -i http://example.com/

# Second request: X-Cache: HIT, with an Age header
curl -x http://127.0.0.1:8080 -i http://example.com/

# Force revalidation with the origin
curl -x http://127.0.0.1:8080 -i -H 'Cache-Control: no-cache' http://example.com/

# Answer from cache only (504 if not cached)
curl -x http://127.0.0.1:8080 -i -H 'Cache-Control: only-if-cached' http://example.com/other

# HTTPS goes through a CONNECT tunnel (not cached)
curl -x http://127.0.0.1:8080 https://example.com/
```

### With environment variables

```bash
export http_proxy=http://127.0.0.1:8080
export https_proxy=http://127.0.0.1:8080
curl -i http://example.com/
```

### In a browser

Configure a manual HTTP proxy at `127.0.0.1` port `8080` and use the same proxy for HTTPS. In Firefox: *Settings → Network Settings → Settings…*.

### `X-Cache` header

| Value | Meaning |
| --- | --- |
| `HIT` | served from cache without contacting the origin |
| `MISS` | fetched from the origin (and stored if policy allows) |
| `REVALIDATED` | stale entry confirmed by the origin with `304 Not Modified` |

Responses to methods other than `GET` carry no `X-Cache` header.

### Statistics endpoint

```bash
curl http://127.0.0.1:8080/__proxy/stats
```

```json
{
  "connections": 12,
  "active_connections": 1,
  "requests": 20,
  "tunnels": 2,
  "upstream_errors": 0,
  "cache": {
    "enabled": true,
    "entries": 5,
    "bytes": 48213,
    "capacity": 67108864,
    "hits": 9,
    "misses": 6,
    "revalidations": 1,
    "stores": 5,
    "evictions": 0,
    "invalidations": 0
  }
}
```

### Errors generated by the proxy

| Status | When |
| --- | --- |
| `400` | malformed request, target that is not an absolute `http://` URL, or CONNECT without `host:port` |
| `403` | CONNECT to a port not in `--connect-ports` |
| `413` | body larger than the limit |
| `431` | headers larger than 64 KiB or more than 100 header fields |
| `502` | origin does not resolve, refuses the connection or sends an invalid response |
| `504` | upstream timeout, or `only-if-cached` with nothing in the cache |
| `505` | unsupported HTTP version |
| `508` | loop detected (the request already went through this proxy) |

## 6. Tests

### Unit tests

```bash
ctest --test-dir build --output-on-failure
```

Or run the Catch2 executable directly, which lets you filter by name or tag:

```bash
./build/proxy_tests                    # everything
./build/proxy_tests "[parser]"         # a single tag
./build/proxy_tests --list-tests       # list test cases
```

| File | Covers |
| --- | --- |
| `tests/test_parser.cpp` | HTTP/1.1 parser: framing, chunked, pipelining, limits, errors |
| `tests/test_url.cpp` | URL parsing and normalization, reference resolution |
| `tests/test_date.cpp` | HTTP dates (IMF-fixdate, RFC 850, asctime) |
| `tests/test_lru_cache.cpp` | cost-bounded LRU, eviction |
| `tests/test_cache_control.cpp` | `Cache-Control` directive parsing |
| `tests/test_http_cache.cpp` | RFC 9111 policy: storage, freshness, revalidation, `Vary` |

### Smoke test (end to end)

`scripts/smoke_test.sh` starts a local origin (`python3 -m http.server`) and the proxy, then checks forwarding, `MISS → HIT`, the stats endpoint, the CONNECT port policy and the `502` error path:

```bash
scripts/smoke_test.sh build/http_proxy
```

It needs `bash`, `curl` and `python3`. Ports can be changed with `PROXY_PORT` and `ORIGIN_PORT`.

## 7. Continuous integration

The [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) workflow runs on every push to `main`, on every pull request and on demand (*workflow_dispatch*). On Ubuntu 24.04 it runs a four-job matrix:

| Compiler | Build | Sanitizers |
| --- | --- | --- |
| GCC | Release | — |
| Clang | Release | — |
| GCC | Debug | ASan + UBSan |
| Clang | Debug | ASan + UBSan |

Each job builds with warnings as errors, then runs the unit tests and the smoke test. To reproduce a job locally:

```bash
CXX=clang++ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPROXY_ENABLE_SANITIZERS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
scripts/smoke_test.sh build/http_proxy
```

## 8. Troubleshooting

**`Could NOT find Boost`**: install the Boost development package (`libboost-dev`, `boost-devel`, `boost`). For a non-standard location, pass `-DBOOST_ROOT=/path/to/boost`.

**Catch2 download fails**: configuring with tests needs access to GitHub. Offline, use `-DPROXY_BUILD_TESTS=OFF`.

**`bind: Address already in use`**: another program uses the port. Pick another one with `--port`.

**curl bypasses the proxy for `localhost`/`127.0.0.1`**: `NO_PROXY`/`no_proxy` takes precedence over `-x`. Use `curl --noproxy '' -x ...`.

**`400 only http:// URLs can be proxied`**: the client sent an `https://` URL without CONNECT. Configure the client to use the proxy for HTTPS as well.

**`403 CONNECT to port N is not allowed`**: add the port to `--connect-ports` (e.g. `--connect-ports 443,8443`).

**Linker cannot find `libclang_rt.asan`**: install Clang's `compiler-rt` (`libclang-rt-dev` on Ubuntu) or use GCC.

by caiothedev<br />
#docs