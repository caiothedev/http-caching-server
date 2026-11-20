# HTTP CACHING - ᴇɴ

This document describes the in-memory shared HTTP cache implemented by the proxy. It focuses on actual behavior rather than every feature defined by RFC 9111.

- [Project documentation](README.md)
- [Architecture](ARCHITECTURE.en.md)
- [Português (Brasil)](CACHING.pt-br.md)

## 1. Overview

The cache is implemented by `HttpCache` and a byte-bounded `LruCache`. It is shared by all client sessions in one server process and protected by a mutex.

- Cache key: normalized absolute HTTP URL, including its path and query.
- Stored data: response status, reason, headers, and decoded body.
- Storage: process memory only; entries are lost when the process exits.
- Eviction: least recently used (LRU), bounded by total estimated entry size.
- Cache scope: shared cache; it is not partitioned by client identity.

HTTPS traffic carried through `CONNECT` is an opaque TCP tunnel and is not cached.

## 2. Request eligibility

Only ordinary `GET` requests are looked up in the cache. Requests carrying client conditional headers (such as `If-None-Match`, `If-Modified-Since`, `If-Match`, `If-Unmodified-Since`, `If-Range`, or `Range`) bypass the cache lookup and are forwarded.

A request with `Cache-Control: no-store` is not eligible for storage. A request with `Cache-Control: no-cache` or, when `Cache-Control` is absent, `Pragma: no-cache` requires validation of an existing entry.

## 3. Response storage rules

A response is stored only when all applicable conditions pass:

- The request method is `GET`.
- The response status is not informational (`1xx`), partial content (`206`), or `304`.
- Neither request nor response has `Cache-Control: no-store`.
- The response does not have `Cache-Control: private`, because this is a shared cache.
- If the request contains `Authorization`, the response must explicitly permit shared storage with `public`, `s-maxage`, or `must-revalidate`.
- A response containing `Set-Cookie` is stored only when it also has `Cache-Control: public`.
- `Vary: *` responses are never stored.
- The response must have explicit freshness information (`max-age`, `s-maxage`, `Expires`, or `public`), or a heuristically cacheable status with an `ETag` or `Last-Modified` validator.
- The response must fit within the configured maximum object size, including the cache's estimated metadata overhead.

The statuses eligible for heuristic caching by default are `200`, `203`, `204`, `300`, `301`, `308`, `404`, `405`, `410`, `414`, and `501`. Without explicit freshness, a validator is also required.

## 4. Freshness and age

Freshness lifetime is selected in this order:

1. `s-maxage`
2. `max-age`
3. `Expires - Date` (or the response receipt time when `Date` is absent)
4. A heuristic lifetime equal to 10% of `Date - Last-Modified`, only for the statuses listed above and capped by the configured heuristic maximum.

An invalid `Expires` date produces a zero freshness lifetime. The default heuristic maximum is 24 hours.

The current age is calculated using the corrected initial age and the time elapsed since the response was received. The corrected initial age accounts for the `Date` and `Age` response fields and the response delay.

An entry is fresh while its current age is lower than its freshness lifetime, subject to request directives. An entry marked `no-cache` must be validated before reuse. `must-revalidate`, `proxy-revalidate`, and `s-maxage` prevent serving a stale response under the implemented `max-stale` handling.

## 5. Request directives

The implementation recognizes these request directives:

| Directive | Implemented behavior |
| --- | --- |
| `no-cache` | Requires validation before reusing an existing entry. |
| `no-store` | Prevents storing the response to this request. |
| `max-age=N` | Rejects an entry whose current age is at least N seconds. |
| `min-fresh=N` | Requires at least N seconds of freshness to remain. |
| `max-stale` | Allows a stale entry when the response does not require revalidation. |
| `max-stale=N` | Allows staleness up to N seconds under the same restriction. |
| `only-if-cached` | Does not contact the origin on a cache miss; the proxy returns `504`. |

For HTTP/1.0 compatibility, `Pragma: no-cache` is treated as `no-cache` only when the request has no `Cache-Control` field.

## 6. Response directives and metadata

The parser recognizes `no-cache`, `no-store`, `no-transform`, `max-age`, `s-maxage`, `must-revalidate`, `proxy-revalidate`, `public`, and `private`. Unknown directives are ignored. Qualified `no-cache="..."` and `private="..."` forms are handled as their unqualified forms.

The cache also uses:

- `Date`, `Age`, and `Expires` to calculate age and freshness.
- `ETag` and `Last-Modified` as revalidation validators.
- `Vary` to associate an entry with the request-header values used to select the representation.

For `Vary`, the cache stores the combined value of each named request header and checks it on lookup. A mismatch is a cache miss.

## 7. Revalidation

When an entry is stale or requires validation, the proxy sends a conditional request to the origin using the stored `ETag` and/or `Last-Modified` values:

- `If-None-Match` for `ETag`.
- `If-Modified-Since` for `Last-Modified`.

If the origin returns `304 Not Modified`, the proxy merges the applicable response headers into the stored response, recalculates cache metadata, and serves the stored body. The client receives the original representation with an updated `Age` header and `X-Cache: REVALIDATED`.

If the origin returns a new response, that response is evaluated for storage and replaces or invalidates the stale entry as appropriate.

## 8. Invalidation

After a successful unsafe request (a method other than `GET` or `HEAD`, with a response status below `400`), the proxy invalidates:

- The cache entry for the request target.
- Same-origin URIs referenced by the response's `Location` and `Content-Location` fields, when those values can be resolved.

The cache does not persist entries across restarts.

## 9. Capacity and eviction

The default total cache capacity is 64 MiB and the default maximum object size is 8 MiB. Both are configurable from the command line.

The LRU tracks estimated bytes, including the body, response reason, header names and values, and a fixed per-entry overhead. When inserting an entry would exceed the total capacity, least recently used entries are evicted until the new entry fits. An entry larger than the maximum object size or total cache capacity is not stored.

The maximum object size is clamped to the total cache capacity during configuration.

## 10. Client-visible indicators

| Header | Value | Meaning |
| --- | --- | --- |
| `X-Cache` | `HIT` | A fresh stored response was served without contacting the origin. |
| `X-Cache` | `MISS` | The request was fetched from the origin; storage is attempted when caching is enabled. |
| `X-Cache` | `REVALIDATED` | A stale entry was validated by the origin with `304 Not Modified`. |
| `Age` | seconds | Current calculated age of a response served from the cache. |

Responses to methods other than `GET` do not receive an `X-Cache` header. A `MISS` does not guarantee that the response was stored.

## 11. Limitations

- The cache is in-memory and local to one process; there is no disk persistence or distributed cache.
- Only `GET` responses are cached.
- `CONNECT` tunnels are not cached or inspected.
- Range/conditional client requests bypass cache lookup.
- This is a deliberately scoped implementation of shared-cache behavior, not a claim of complete RFC 9111 coverage.

by caiothedev<br />
#caching