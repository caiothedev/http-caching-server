#!/usr/bin/env bash
# End-to-end smoke test: starts a local origin (python http.server) and the
# proxy, then checks forwarding, caching (MISS -> HIT), the stats endpoint and
# the CONNECT port policy.
#
# Usage: scripts/smoke_test.sh [path/to/http_proxy]
set -euo pipefail

PROXY_BIN="${1:-./build/http_proxy}"
PROXY_PORT="${PROXY_PORT:-18080}"
ORIGIN_PORT="${ORIGIN_PORT:-18081}"
PROXY="http://127.0.0.1:${PROXY_PORT}"
ORIGIN="http://127.0.0.1:${ORIGIN_PORT}"

WORK_DIR="$(mktemp -d)"
PIDS=()

cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    echo "---- proxy log ----" >&2
    cat "$WORK_DIR/proxy.log" >&2 || true
    exit 1
}

# --noproxy '' makes curl ignore NO_PROXY/no_proxy, which often lists 127.0.0.1.
pcurl() { curl --noproxy '' -sS --max-time 10 -x "$PROXY" "$@"; }

wait_for_port() {
    for _ in $(seq 1 50); do
        if curl --noproxy '' -s -o /dev/null "$1"; then return 0; fi
        sleep 0.1
    done
    return 1
}

[[ -x "$PROXY_BIN" ]] || { echo "proxy binary not found: $PROXY_BIN" >&2; exit 2; }

# Origin: a static file whose Last-Modified is in the past, so the proxy can
# apply heuristic freshness and serve the second request from cache.
mkdir -p "$WORK_DIR/www"
echo "hello from origin" > "$WORK_DIR/www/index.html"
touch -d '2 days ago' "$WORK_DIR/www/index.html"
python3 -m http.server "$ORIGIN_PORT" --bind 127.0.0.1 --directory "$WORK_DIR/www" \
    > "$WORK_DIR/origin.log" 2>&1 &
PIDS+=($!)

"$PROXY_BIN" --port "$PROXY_PORT" --threads 2 --log-level debug > "$WORK_DIR/proxy.log" 2>&1 &
PIDS+=($!)

wait_for_port "$ORIGIN/index.html" || fail "origin did not start"
wait_for_port "$PROXY/__proxy/stats" || fail "proxy did not start"

echo "1. forwarding + cache MISS"
headers="$(pcurl -D - -o "$WORK_DIR/body1" "$ORIGIN/index.html")"
grep -q "hello from origin" "$WORK_DIR/body1" || fail "unexpected body"
grep -qi '^X-Cache: MISS' <<<"$headers" || fail "expected X-Cache: MISS, got: $headers"
grep -qi '^Via: .*http-caching-proxy' <<<"$headers" || fail "missing Via header"

echo "2. cache HIT"
headers="$(pcurl -D - -o "$WORK_DIR/body2" "$ORIGIN/index.html")"
cmp -s "$WORK_DIR/body1" "$WORK_DIR/body2" || fail "cached body differs"
grep -qi '^X-Cache: HIT' <<<"$headers" || fail "expected X-Cache: HIT, got: $headers"
grep -qi '^Age: ' <<<"$headers" || fail "missing Age header on HIT"

echo "3. stats endpoint"
stats="$(curl --noproxy '' -sS "$PROXY/__proxy/stats")"
grep -q '"hits": 1' <<<"$stats" || fail "unexpected stats: $stats"

echo "4. CONNECT to a disallowed port is refused"
code="$(pcurl -o /dev/null -w '%{http_connect}' -p "$ORIGIN/" 2>/dev/null || true)"
[[ "$code" == "403" ]] || fail "expected 403 for CONNECT to port $ORIGIN_PORT, got $code"

echo "5. upstream connection refused -> 502"
code="$(pcurl -o /dev/null -w '%{http_code}' "http://127.0.0.1:1/" || true)"
[[ "$code" == "502" ]] || fail "expected 502 for unreachable origin, got $code"

echo "smoke test passed"