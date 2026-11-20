# CONFIGURATION - ᴇɴ

This document lists the command-line options, defaults, accepted values, and runtime behavior of the HTTP Caching Proxy Server.

- [Project documentation](README.md)
- [Architecture](ARCHITECTURE.en.md)
- [Português (Brasil)](CONFIGURATION.pt-br.md)

## 1. Quick start

Run with the default configuration:

```bash
./build/http_proxy
```

Example with explicit settings:

```bash
./build/http_proxy --listen 127.0.0.1 --port 3128 --threads 4 \
  --cache-size 256 --max-object 16 --timeout 30 \
  --idle-timeout 60 --tunnel-timeout 300 --connect-ports 443 \
  --log-level info
```

Options that take a value accept both forms:

```bash
--port 8080
--port=8080
```

## 2. Command-line options

| Option | Default | Accepted values / range | Description |
| --- | --- | --- | --- |
| `-l, --listen ADDR` | `127.0.0.1` | IPv4 or IPv6 address | Local address on which the server listens. |
| `-p, --port N` | `8080` | 1–65535 | TCP listening port. |
| `-t, --threads N` | CPU count | 1–1024 | Number of worker threads running the Asio event loop. |
| `--cache-size MB` | `64` | 1–1,048,576 MiB | Total cache capacity. |
| `--max-object MB` | `8` | 1–1,048,576 MiB | Maximum estimated cache entry size, clamped to cache capacity and body limit. |
| `--max-connections N` | `16` | 1–1024 | Maximum simultaneous client connections, including CONNECT tunnels. |
| `--max-body MB` | `8` | 1–128 MiB | Maximum individual request/response body size. The product of this value and `--max-connections` cannot exceed 128 MiB. |
| `--no-cache` | disabled | flag | Disables cache lookup and storage; requests are forwarded. |
| `--timeout SEC` | `30` | 1–3600 seconds | Maximum upstream total duration and inactivity period; both use this timeout. |
| `--idle-timeout SEC` | `60` | 1–3600 seconds | Idle client keep-alive timeout. |
| `--tunnel-timeout SEC` | `300` | 1–86400 seconds | Idle CONNECT tunnel timeout. |
| `--connect-ports LIST` | `443` | Comma-separated ports 1–65535, or `any` | Destination ports permitted for CONNECT. |
| `--log-level LEVEL` | `info` | `debug`, `info`, `warn`, `error` | Minimum log severity. |
| `-h, --help` | — | flag | Prints usage information and exits. |

## 3. Defaults and derived values

When `--threads` is omitted, the server uses the reported hardware concurrency, with a minimum of one worker thread.

The cache defaults are:

- Total capacity: 64 MiB.
- Maximum cache entry: 8 MiB.
- Maximum heuristic freshness lifetime: 24 hours (internal default; not currently exposed as a CLI option).

The HTTP parser defaults are:

- Maximum start-line plus header section: 64 KiB.
- Maximum header fields: 100.
- Maximum message body: 8 MiB.

The server admits up to 16 simultaneous client connections by default, including CONNECT tunnels. `--max-connections` and `--max-body` are validated together so their product cannot exceed 128 MiB of configured in-flight message-body capacity. Fixed `Content-Length` values are validated without reserving the full declared body before the payload arrives.

The effective body limit is raised when the configured maximum cache entry is larger than the parser's default body limit. The maximum cache entry size is also clamped to the total cache capacity when it is larger.

## 4. CONNECT port policy

By default, CONNECT requests may reach destination port `443` only.

For example, to allow ports 443 and 8443:

```bash
./build/http_proxy --connect-ports 443,8443
```

To allow any destination port:

```bash
./build/http_proxy --connect-ports any
```

An empty port list is interpreted internally as allowing any port. The `any` setting is not recommended for an exposed proxy. This option restricts ports only; it does not provide a hostname or destination-IP allowlist.

## 5. Cache configuration

Use `--cache-size` to set the total in-memory cache capacity and `--max-object` to set the maximum size of an individual entry.

```bash
./build/http_proxy --cache-size 512 --max-object 32
```

Values are specified in MiB. The parser accepts each value from 1 to 1,048,576 MiB. If `--max-object` exceeds `--cache-size`, it is reduced to the total cache capacity.

Disable the cache entirely with:

```bash
./build/http_proxy --no-cache
```

See [CACHING.md](CACHING.en.md) for the storage rules, freshness calculation, revalidation, and eviction behavior.

## 6. Logging

The default level is `info`.

- `debug`: detailed connection, forwarding, and cache diagnostics.
- `info`: listening information and request access lines.
- `warn`: recoverable problems and rejected conditions.
- `error`: fatal errors.

Access log lines include the client endpoint, request method and URL, response status, response size, cache result, and elapsed time. Treat logs as potentially sensitive because URLs may contain private data.

## 7. Validation and errors

Invalid or missing option values, unknown options, and out-of-range numeric values cause the program to print an error and usage text, then exit with status code `2`.

The listening address is parsed when the server starts. An invalid address or a socket setup failure is reported as a startup error.

Use:

```bash
./build/http_proxy --help
```

to print the available options.

## 8. Security notes

The default listener is `127.0.0.1`. Binding to `0.0.0.0` exposes the proxy on all IPv4 interfaces. The server has no built-in client authentication or access-control list.

Do not expose the proxy to an untrusted network without external network controls. See [SECURITY.md](../SECURITY.en.md) before changing the listener address or CONNECT policy.

by caiothedev<br />
#configuration