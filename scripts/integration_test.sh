#!/usr/bin/env bash
# Integration tests for 304 revalidation, unsafe-method invalidation, Vary,
# Expect: 100-continue, client keep-alive, and CONNECT tunnelling.
set -euo pipefail
PROXY_BIN="\${1:-./build/http_proxy}"
PROXY_PORT="\${PROXY_PORT:-18180}"
ORIGIN_PORT="\${ORIGIN_PORT:-18181}"
ECHO_PORT="\${ECHO_PORT:-18443}"
PROXY="http://127.0.0.1:\${PROXY_PORT}"
ORIGIN="http://127.0.0.1:\${ORIGIN_PORT}"
WORK_DIR="$(mktemp -d)"
SERVER_PID=""
PROXY_PID=""
cleanup() {
    [[ -z "$PROXY_PID" ]] || kill "$PROXY_PID" 2>/dev/null || true
    [[ -z "$SERVER_PID" ]] || kill "$SERVER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT
fail() {
    echo "FAIL: $*" >&2
    [[ ! -f "$WORK_DIR/proxy.log" ]] || cat "$WORK_DIR/proxy.log" >&2
    [[ ! -f "$WORK_DIR/origin.log" ]] || cat "$WORK_DIR/origin.log" >&2
    exit 1
}
[[ -x "$PROXY_BIN" ]] || { echo "proxy binary not found: $PROXY_BIN" >&2; exit 2; }
command -v curl >/dev/null
command -v python3 >/dev/null

cat > "$WORK_DIR/server.py" <<'PY'
import http.server
import socketserver
import sys
import threading

origin_port, echo_port = map(int, sys.argv[1:3])
state = {"resource": 1}

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, fmt, *args):
        print(fmt % args, flush=True)
    def send_body(self, status, body=b"", headers=()):
        self.send_response(status)
        self.send_header("Date", self.date_time_string())
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)
    def do_GET(self):
        if self.path == "/revalidate":
            headers = [("Cache-Control", "no-cache, max-age=0"), ("ETag", '"v1"')]
            if self.headers.get("If-None-Match") == '"v1"':
                self.send_body(304, headers=headers)
            else:
                self.send_body(200, b"cached-revalidation-body", headers)
        elif self.path == "/invalidate":
            self.send_body(200, f"version-{state['resource']}".encode(), [("Cache-Control", "max-age=600")])
        elif self.path == "/vary":
            language = self.headers.get("Accept-Language", "")
            self.send_body(200, language.encode(), [("Cache-Control", "max-age=600"), ("Vary", "Accept-Language")])
        else:
            self.send_body(404, b"not found")
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path == "/invalidate":
            state["resource"] += 1
            self.send_body(204)
        elif self.path == "/echo":
            self.send_body(200, body, [("Cache-Control", "no-store")])
        else:
            self.send_body(404, b"not found")

class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(4096)
            if not data:
                return
            self.request.sendall(data)

class ThreadingTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

origin = http.server.ThreadingHTTPServer(("127.0.0.1", origin_port), Handler)
echo = ThreadingTCPServer(("127.0.0.1", echo_port), Echo)
threading.Thread(target=origin.serve_forever, daemon=True).start()
threading.Thread(target=echo.serve_forever, daemon=True).start()
threading.Event().wait()
PY

python3 "$WORK_DIR/server.py" "$ORIGIN_PORT" "$ECHO_PORT" > "$WORK_DIR/origin.log" 2>&1 &
SERVER_PID=$!
"$PROXY_BIN" --listen 127.0.0.1 --port "$PROXY_PORT" --threads 2 --timeout 3 \
    --connect-ports "$ECHO_PORT" --log-level debug > "$WORK_DIR/proxy.log" 2>&1 &
PROXY_PID=$!
for _ in $(seq 1 60); do
    curl --noproxy '' -sS --max-time 1 "$PROXY/__proxy/stats" >/dev/null 2>&1 && break
    sleep 0.1
done
curl --noproxy '' -sS --max-time 2 "$PROXY/__proxy/stats" >/dev/null || fail "proxy did not start"
pcurl() { curl --noproxy '' -sS --max-time 10 -x "$PROXY" "$@"; }

echo "1. conditional revalidation with 304"
pcurl -D "$WORK_DIR/revalidate1.headers" -o "$WORK_DIR/revalidate1.body" "$ORIGIN/revalidate"
pcurl -D "$WORK_DIR/revalidate2.headers" -o "$WORK_DIR/revalidate2.body" "$ORIGIN/revalidate"
grep -qi '^X-Cache: REVALIDATED' "$WORK_DIR/revalidate2.headers" || fail "expected REVALIDATED"
cmp -s "$WORK_DIR/revalidate1.body" "$WORK_DIR/revalidate2.body" || fail "304 lost cached body"

echo "2. successful POST invalidates cached GET"
pcurl -D "$WORK_DIR/get1.headers" -o "$WORK_DIR/get1.body" "$ORIGIN/invalidate"
grep -q 'version-1' "$WORK_DIR/get1.body" || fail "unexpected initial version"
pcurl -X POST -o /dev/null "$ORIGIN/invalidate" || fail "POST failed"
pcurl -D "$WORK_DIR/get2.headers" -o "$WORK_DIR/get2.body" "$ORIGIN/invalidate"
grep -qi '^X-Cache: MISS' "$WORK_DIR/get2.headers" || fail "expected MISS after invalidation"
grep -q 'version-2' "$WORK_DIR/get2.body" || fail "resource was not invalidated"

echo "3. Vary selection and current single-variant replacement behavior"
pcurl -H 'Accept-Language: en' -o /dev/null "$ORIGIN/vary"
pcurl -D "$WORK_DIR/vary-hit.headers" -H 'Accept-Language: en' -o /dev/null "$ORIGIN/vary"
grep -qi '^X-Cache: HIT' "$WORK_DIR/vary-hit.headers" || fail "expected Vary HIT"
pcurl -D "$WORK_DIR/vary-fr.headers" -H 'Accept-Language: fr' -o "$WORK_DIR/vary-fr.body" "$ORIGIN/vary"
grep -qi '^X-Cache: MISS' "$WORK_DIR/vary-fr.headers" || fail "expected Vary mismatch MISS"
grep -q '^fr$' "$WORK_DIR/vary-fr.body" || fail "wrong Vary response"
pcurl -D "$WORK_DIR/vary-en-again.headers" -H 'Accept-Language: en' -o /dev/null "$ORIGIN/vary"
grep -qi '^X-Cache: MISS' "$WORK_DIR/vary-en-again.headers" || fail "expected previous variant to be replaced"

echo "4. POST body and Expect: 100-continue"
pcurl -H 'Expect: 100-continue' --data-binary 'integration-payload' "$ORIGIN/echo" \
    -o "$WORK_DIR/echo.body" || fail "Expect/POST exchange failed"
grep -q '^integration-payload$' "$WORK_DIR/echo.body" || fail "POST body changed in transit"

echo "5. multiple requests on one client connection"
python3 - "$PROXY_PORT" "$ORIGIN_PORT" <<'PY'
import socket, sys
proxy_port, origin_port = map(int, sys.argv[1:3])
sock = socket.create_connection(("127.0.0.1", proxy_port), timeout=5)
sock.settimeout(5)
def response():
    data = bytearray()
    while b"\r\n\r\n" not in data:
        data.extend(sock.recv(4096))
    head, body = data.split(b"\r\n\r\n", 1)
    headers = {}
    for line in head.split(b"\r\n")[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.lower()] = v.strip()
    size = int(headers.get(b"content-length", b"0"))
    while len(body) < size:
        body += sock.recv(4096)
    return head.split(b"\r\n", 1)[0], body[:size]
for mode in ("keep-alive", "close"):
    sock.sendall((f"GET http://127.0.0.1:{origin_port}/vary HTTP/1.1\r\n"
                  f"Host: 127.0.0.1:{origin_port}\r\nAccept-Language: persistent\r\n"
                  f"Connection: {mode}\r\n\r\n").encode())
    status, body = response()
    if b"200" not in status or body != b"persistent":
        raise RuntimeError(f"bad persistent response: {status!r}, {body!r}")
sock.close()
PY

echo "6. allowed CONNECT tunnel is bidirectional"
python3 - "$PROXY_PORT" "$ECHO_PORT" <<'PY'
import socket, sys
proxy_port, echo_port = map(int, sys.argv[1:3])
sock = socket.create_connection(("127.0.0.1", proxy_port), timeout=5)
sock.settimeout(5)
sock.sendall((f"CONNECT 127.0.0.1:{echo_port} HTTP/1.1\r\n"
              f"Host: 127.0.0.1:{echo_port}\r\n\r\n").encode())
head = bytearray()
while b"\r\n\r\n" not in head:
    head.extend(sock.recv(4096))
if not head.startswith(b"HTTP/1.1 200"):
    raise RuntimeError(f"CONNECT refused: {head!r}")
payload = b"connect-tunnel-roundtrip"
sock.sendall(payload)
received = b""
while len(received) < len(payload):
    received += sock.recv(len(payload) - len(received))
if received != payload:
    raise RuntimeError("CONNECT tunnel corrupted payload")
sock.close()
PY

echo "integration test passed"