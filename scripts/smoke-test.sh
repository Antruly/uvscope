#!/usr/bin/env bash
# End-to-end smoke test used by CI and release verification.
set -euo pipefail

BIN="${1:-./build/uvscope}"
PORT="${2:-18787}"
PROXY_PORT="${3:-18788}"
TCP_PORT="${4:-18789}"
TCP_LISTEN_PORT="${5:-18790}"
UDP_LISTEN_PORT="${6:-18791}"
SOCKS5_PORT="${7:-18792}"
SOCKS5_AUTH_PORT="${8:-18793}"
SOCKS5_UDP_TARGET_PORT="${9:-18794}"
H2_PORT="${10:-18795}"
COMP_PORT="${11:-18796}"
WS_PORT="${12:-18797}"
MULTI_PORT="${13:-18798}"
DEAD_PORT="${14:-18799}"
DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/uvscope-smoke.XXXXXX")"
COOKIE_JAR="${DATA_DIR}/cookies.txt"
LOG="${DATA_DIR}/daemon.log"
PID=""
ECHO_PID=""
H2_PID=""
COMP_PID=""
WS_PID=""

cleanup() {
  if [[ -n "${WS_PID}" ]] && kill -0 "${WS_PID}" 2>/dev/null; then
    kill "${WS_PID}" 2>/dev/null || true
    wait "${WS_PID}" 2>/dev/null || true
  fi
  if [[ -n "${COMP_PID}" ]] && kill -0 "${COMP_PID}" 2>/dev/null; then
    kill "${COMP_PID}" 2>/dev/null || true
    wait "${COMP_PID}" 2>/dev/null || true
  fi
  if [[ -n "${H2_PID}" ]] && kill -0 "${H2_PID}" 2>/dev/null; then
    kill "${H2_PID}" 2>/dev/null || true
    wait "${H2_PID}" 2>/dev/null || true
  fi
  if [[ -n "${ECHO_PID}" ]] && kill -0 "${ECHO_PID}" 2>/dev/null; then
    kill "${ECHO_PID}" 2>/dev/null || true
    wait "${ECHO_PID}" 2>/dev/null || true
  fi
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill "${PID}" 2>/dev/null || true
    for _ in $(seq 1 30); do
      kill -0 "${PID}" 2>/dev/null || break
      sleep 0.1
    done
    kill -9 "${PID}" 2>/dev/null || true
  fi
  rm -rf "${DATA_DIR}"
}
trap cleanup EXIT

UVSCOPE_DATA_DIR="${DATA_DIR}" "${BIN}" --host 127.0.0.1 --port "${PORT}" >"${LOG}" 2>&1 &
PID=$!

for _ in $(seq 1 80); do
  if curl -fsS --max-time 1 "http://127.0.0.1:${PORT}/api/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "${PID}" 2>/dev/null; then
    echo "daemon exited during startup" >&2
    cat "${LOG}" >&2
    exit 1
  fi
  sleep 0.1
done

# Local TCP echo server for the persistent-session assertions.
cat >"${DATA_DIR}/echo.py" <<'PY'
import socket
import sys
import threading

port = int(sys.argv[1])
udp_port = int(sys.argv[2]) if len(sys.argv) > 2 else 0
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(16)
print("LISTENING", flush=True)


def handle(conn):
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            conn.sendall(b"echo:" + data)
    except OSError:
        pass
    finally:
        conn.close()


if udp_port:
    udp_echo = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_echo.bind(("127.0.0.1", udp_port))

    def handle_udp():
        while True:
            data, peer = udp_echo.recvfrom(65536)
            udp_echo.sendto(b"udp:" + data, peer)

    threading.Thread(target=handle_udp, daemon=True).start()

while True:
    connection, _ = server.accept()
    threading.Thread(target=handle, args=(connection,), daemon=True).start()
PY
python3 "${DATA_DIR}/echo.py" "${TCP_PORT}" "${SOCKS5_UDP_TARGET_PORT}" >"${DATA_DIR}/echo.log" 2>&1 &
ECHO_PID=$!
for _ in $(seq 1 80); do
  if grep -q LISTENING "${DATA_DIR}/echo.log" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

# Minimal WebSocket echo server. The handshake requires X-Smoke: ok, which also
# proves that the bundled libuvcpp handshake-header patch is applied.
cat >"${DATA_DIR}/ws_echo.py" <<'WSPY'
import base64, hashlib, socket, struct, sys, threading
GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'

def read_frame(conn):
    head = conn.recv(2)
    if len(head) < 2:
        return None
    b1, b2 = head
    opcode = b1 & 0x0f
    masked = b2 >> 7
    length = b2 & 0x7f
    if length == 126:
        length = struct.unpack('!H', conn.recv(2))[0]
    elif length == 127:
        length = struct.unpack('!Q', conn.recv(8))[0]
    mask = conn.recv(4) if masked else b''
    payload = b''
    while len(payload) < length:
        chunk = conn.recv(length - len(payload))
        if not chunk:
            return None
        payload += chunk
    if masked:
        payload = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))
    return opcode, payload

def send_frame(conn, opcode, payload):
    b1 = 0x80 | opcode
    n = len(payload)
    if n < 126:
        head = struct.pack('!BB', b1, n)
    elif n < 65536:
        head = struct.pack('!BBH', b1, 126, n)
    else:
        head = struct.pack('!BBQ', b1, 127, n)
    conn.sendall(head + payload)

def handle(conn):
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return
        data += chunk
    headers = {}
    for line in data.decode('latin1').split('\r\n')[1:]:
        if ':' in line:
            key, value = line.split(':', 1)
            headers[key.strip().lower()] = value.strip()
    if headers.get('x-smoke') != 'ok':
        conn.sendall(b'HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n')
        conn.close()
        return
    accept = base64.b64encode(hashlib.sha1(
        (headers.get('sec-websocket-key', '') + GUID).encode()).digest()).decode()
    conn.sendall(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n'
                  'Connection: Upgrade\r\nSec-WebSocket-Accept: ' + accept +
                  '\r\n\r\n').encode())
    while True:
        frame = read_frame(conn)
        if frame is None:
            break
        opcode, payload = frame
        if opcode in (0x1, 0x2):
            send_frame(conn, opcode, b'echo:' + payload)
        elif opcode == 0x9:
            send_frame(conn, 0xA, payload)
        elif opcode == 0x8:
            send_frame(conn, 0x8, payload)
            break
    conn.close()

server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', int(sys.argv[1])))
server.listen(16)
print('WS LISTENING', flush=True)
while True:
    conn, _ = server.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
WSPY
python3 "${DATA_DIR}/ws_echo.py" "${WS_PORT}" >"${DATA_DIR}/ws.log" 2>&1 &
WS_PID=$!
for _ in $(seq 1 80); do
  if grep -q "WS LISTENING" "${DATA_DIR}/ws.log" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

curl -fsS "http://127.0.0.1:${PORT}/api/health" >/dev/null
curl -fsS -c "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/setup/init" \
  -H 'content-type: application/json' \
  -d '{"username":"ci-admin","password":"ci-admin-password"}' >/dev/null

curl -fsS -b "${COOKIE_JAR}" "http://127.0.0.1:${PORT}/api/dashboard" >/dev/null

HTTP_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/http/send" \
  -H 'content-type: application/json' \
  -d "{\"url\":\"http://127.0.0.1:${PORT}/api/health\",\"method\":\"GET\",\"protocol\":\"http1\",\"headers\":[],\"body\":\"\",\"timeoutMs\":5000}")"

# HTTP/2 over TLS (ALPN) against a local Node.js h2 server. Self-signed
# certificates are generated per run and never checked in.
H2_RESULT=""
if command -v node >/dev/null 2>&1 && command -v openssl >/dev/null 2>&1; then
  H2_DIR="${DATA_DIR}/h2"
  mkdir -p "${H2_DIR}"
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "${H2_DIR}/key.pem" \
    -out "${H2_DIR}/cert.pem" -days 2 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1
  node "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/h2-test-server.mjs" \
    "${H2_DIR}/key.pem" "${H2_DIR}/cert.pem" "${H2_PORT}" >"${H2_DIR}/server.log" 2>&1 &
  H2_PID=$!
  for _ in $(seq 1 80); do
    if grep -q "H2 LISTENING" "${H2_DIR}/server.log" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  if ! grep -q "H2 LISTENING" "${H2_DIR}/server.log" 2>/dev/null; then
    echo "h2 test server did not start" >&2
    cat "${H2_DIR}/server.log" >&2
    exit 1
  fi
  H2_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/http/send" \
    -H 'content-type: application/json' \
    -d "{\"url\":\"https://127.0.0.1:${H2_PORT}/h2\",\"method\":\"GET\",\"protocol\":\"http2\",\"headers\":[],\"body\":\"\",\"timeoutMs\":8000,\"insecure\":true}")"
else
  echo "skipping HTTP/2 smoke: node or openssl is not available" >&2
fi

# gzip / deflate / brotli decoding and the raw-bytes path.
COMP_GZIP=""
COMP_DEFLATE=""
COMP_BR=""
COMP_RAW=""
COMP_READY=0
if command -v node >/dev/null 2>&1; then
  COMP_DIR="${DATA_DIR}/compression"
  mkdir -p "${COMP_DIR}"
  node "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/compression-test-server.mjs" \
    "${COMP_PORT}" >"${COMP_DIR}/server.log" 2>&1 &
  COMP_PID=$!
  for _ in $(seq 1 80); do
    if grep -q "COMPRESSION LISTENING" "${COMP_DIR}/server.log" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  if ! grep -q "COMPRESSION LISTENING" "${COMP_DIR}/server.log" 2>/dev/null; then
    echo "compression test server did not start" >&2
    cat "${COMP_DIR}/server.log" >&2
    exit 1
  fi
  COMP_READY=1
  for encoding in gzip deflate br; do
    payload="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/http/send" \
      -H 'content-type: application/json' \
      -d "{\"url\":\"http://127.0.0.1:${COMP_PORT}/${encoding}\",\"method\":\"GET\",\"headers\":[],\"body\":\"\",\"timeoutMs\":5000,\"decompress\":true}")"
    if [[ "${encoding}" == "gzip" ]]; then COMP_GZIP="${payload}"; fi
    if [[ "${encoding}" == "deflate" ]]; then COMP_DEFLATE="${payload}"; fi
    if [[ "${encoding}" == "br" ]]; then COMP_BR="${payload}"; fi
  done
  COMP_RAW="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/http/send" \
    -H 'content-type: application/json' \
    -d "{\"url\":\"http://127.0.0.1:${COMP_PORT}/gzip\",\"method\":\"GET\",\"headers\":[],\"body\":\"\",\"timeoutMs\":5000,\"decompress\":false}")"
else
  echo "skipping compression smoke: node is not available" >&2
fi

# The daemon must serve the built SPA, not Vite's dev index (which loads
# /src/main.ts and leaves the UI blank when served statically).
ROOT_HTML="$(curl -fsS "http://127.0.0.1:${PORT}/")"
if grep -q "/src/main.ts" <<<"${ROOT_HTML}"; then
  echo "daemon served the Vite dev index instead of web/dist" >&2
  exit 1
fi
if ! grep -q "/assets/index-" <<<"${ROOT_HTML}"; then
  echo "daemon did not serve the built SPA shell" >&2
  exit 1
fi

# Preview tokens round-trip through /preview/:id.
PREVIEW_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/preview" \
  -H 'content-type: application/json' \
  -d '{"html":"<html><body><p>preview-ok</p></body></html>","baseUrl":"http://127.0.0.1/","mode":"safe"}')"
PREVIEW_ID="$(printf '%s' "${PREVIEW_RESULT}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))')"
if [[ -z "${PREVIEW_ID}" ]]; then
  echo "preview token creation failed: ${PREVIEW_RESULT}" >&2
  exit 1
fi
if ! curl -fsS "http://127.0.0.1:${PORT}/preview/${PREVIEW_ID}" | grep -q "preview-ok"; then
  echo "preview document did not round-trip" >&2
  exit 1
fi
curl -fsS -b "${COOKIE_JAR}" -X DELETE "http://127.0.0.1:${PORT}/api/preview/${PREVIEW_ID}" >/dev/null

# DELETE must remove the terminal, not leave a closing/exited ghost behind.
curl -fsS -b "${COOKIE_JAR}" -X PUT "http://127.0.0.1:${PORT}/api/settings" \
  -H 'content-type: application/json' -d '{"modules":{"terminal":true}}' >/dev/null
TERM_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/terminal/sessions" \
  -H 'content-type: application/json' -d '{"name":"smoke-delete","cols":80,"rows":24}')"
TERM_ID="$(printf '%s' "${TERM_RESULT}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("session",{}).get("id",""))')"
if [[ -z "${TERM_ID}" ]]; then
  echo "terminal creation failed: ${TERM_RESULT}" >&2
  exit 1
fi
curl -fsS -b "${COOKIE_JAR}" -X DELETE "http://127.0.0.1:${PORT}/api/terminal/sessions/${TERM_ID}" >/dev/null
TERM_LIST="$(curl -fsS -b "${COOKIE_JAR}" "http://127.0.0.1:${PORT}/api/terminal/sessions")"
if printf '%s' "${TERM_LIST}" | grep -q "${TERM_ID}"; then
  echo "deleted terminal still appears in the session list" >&2
  exit 1
fi
TERM_CODE="$(curl -sS -o /dev/null -w '%{http_code}' -b "${COOKIE_JAR}" \
  "http://127.0.0.1:${PORT}/api/terminal/sessions/${TERM_ID}")"
if [[ "${TERM_CODE}" != "404" ]]; then
  echo "deleted terminal should return 404, got ${TERM_CODE}" >&2
  exit 1
fi

PROXY_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/proxies" \
  -H 'content-type: application/json' \
  -d "{\"name\":\"smoke\",\"listenHost\":\"127.0.0.1\",\"listenPort\":${PROXY_PORT},\"target\":\"http://127.0.0.1:${PORT}\",\"start\":true,\"rules\":[{\"name\":\"redirect\",\"matchType\":\"exact\",\"pattern\":\"/smoke-redirect\",\"action\":\"redirect\",\"status\":301,\"destination\":\"/api/health\",\"preserveQuery\":true},{\"name\":\"rewrite\",\"matchType\":\"prefix\",\"pattern\":\"/smoke-rewrite\",\"action\":\"rewrite\",\"destination\":\"/api\$rest\",\"preserveQuery\":true},{\"name\":\"method\",\"matchType\":\"exact\",\"pattern\":\"/smoke-method\",\"method\":\"POST\",\"action\":\"redirect\",\"status\":307,\"destination\":\"/api/health\",\"preserveQuery\":false},{\"name\":\"header\",\"matchType\":\"exact\",\"pattern\":\"/smoke-header\",\"headerName\":\"X-Smoke\",\"headerValue\":\"yes\",\"action\":\"redirect\",\"status\":302,\"destination\":\"/api/health\",\"preserveQuery\":false},{\"name\":\"host\",\"matchType\":\"exact\",\"pattern\":\"/smoke-host\",\"host\":\"proxy.test\",\"action\":\"redirect\",\"status\":301,\"destination\":\"/api/health\",\"preserveQuery\":false},{\"name\":\"combined\",\"pattern\":\"\",\"method\":\"POST,PUT\",\"host\":\"proxy.test\",\"headerName\":\"X-Secret\",\"headerValue\":\"1\",\"action\":\"redirect\",\"status\":308,\"destination\":\"/api/health\",\"preserveQuery\":false}]}")"

sleep 0.3
PROXIED="$(curl -fsS --max-time 5 "http://127.0.0.1:${PROXY_PORT}/api/health")"
REDIRECT_HEADERS="$(curl -sS -D - -o /dev/null --max-time 5 "http://127.0.0.1:${PROXY_PORT}/smoke-redirect?x=1")"
REWRITTEN="$(curl -fsS --max-time 5 "http://127.0.0.1:${PROXY_PORT}/smoke-rewrite/health?x=1")"

METHOD_POST="$(curl -sS -o /dev/null -w '%{http_code}|%{redirect_url}' --max-time 5 -X POST "http://127.0.0.1:${PROXY_PORT}/smoke-method")"
METHOD_GET="$(curl -sS -o /dev/null -w '%{http_code}|%{redirect_url}' --max-time 5 "http://127.0.0.1:${PROXY_PORT}/smoke-method")"
HEADER_HIT="$(curl -sS -o /dev/null -w '%{http_code}|%{redirect_url}' --max-time 5 -H 'X-Smoke: yes' "http://127.0.0.1:${PROXY_PORT}/smoke-header")"
HEADER_MISS="$(curl -sS -o /dev/null -w '%{http_code}|%{redirect_url}' --max-time 5 "http://127.0.0.1:${PROXY_PORT}/smoke-header")"
HOST_HIT="$(curl -sS -o /dev/null -w '%{http_code}|%{redirect_url}' --max-time 5 -H 'Host: proxy.test' "http://127.0.0.1:${PROXY_PORT}/smoke-host")"
HOST_MISS="$(curl -sS -o /dev/null -w '%{http_code}|%{redirect_url}' --max-time 5 "http://127.0.0.1:${PROXY_PORT}/smoke-host")"
ALL_HIT="$(curl -sS -o /dev/null -w '%{http_code}|%{redirect_url}' --max-time 5 -X POST -H 'Host: proxy.test' -H 'X-Secret: 1' "http://127.0.0.1:${PROXY_PORT}/smoke-all")"
ALL_MISS="$(curl -sS -o /dev/null -w '%{http_code}|%{redirect_url}' --max-time 5 -X POST -H 'Host: proxy.test' "http://127.0.0.1:${PROXY_PORT}/smoke-all")"
MULTI_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/proxies" \
  -H 'content-type: application/json' \
  -d "{\"name\":\"smoke-multi\",\"listenHost\":\"127.0.0.1\",\"listenPort\":${MULTI_PORT},\"target\":\"http://127.0.0.1:${DEAD_PORT}\",\"upstreams\":[{\"url\":\"http://127.0.0.1:${DEAD_PORT}\",\"weight\":1},{\"url\":\"http://127.0.0.1:${PORT}\",\"weight\":1}],\"retries\":1,\"start\":true,\"rules\":[]}")"
MULTI_STATUSES=""
for _ in $(seq 1 10); do
  MULTI_STATUSES="${MULTI_STATUSES}$(curl -sS -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:${MULTI_PORT}/api/health") "
done

SESSION_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/tcp/sessions" \
  -H 'content-type: application/json' \
  -d "{\"host\":\"127.0.0.1\",\"port\":${TCP_PORT},\"connectTimeoutMs\":3000}")"
SESSION_ID="$(printf '%s' "${SESSION_RESULT}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("session",{}).get("id",""))')"
if [[ -z "${SESSION_ID}" ]]; then
  echo "could not create a TCP session: ${SESSION_RESULT}" >&2
  exit 1
fi
STATE=""
for _ in $(seq 1 50); do
  STATE="$(curl -fsS -b "${COOKIE_JAR}" "http://127.0.0.1:${PORT}/api/tcp/sessions/${SESSION_ID}" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin).get("session",{}).get("state",""))')"
  [[ "${STATE}" == "connected" ]] && break
  sleep 0.1
done
if [[ "${STATE}" != "connected" ]]; then
  echo "TCP session did not connect (state=${STATE})" >&2
  exit 1
fi
SEND_ONE="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/tcp/sessions/${SESSION_ID}/send" \
  -H 'content-type: application/json' -d '{"payload":"smoke-1","encoding":"text"}')"
SEND_TWO="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/tcp/sessions/${SESSION_ID}/send" \
  -H 'content-type: application/json' -d '{"payload":"736d6f6b652d32","encoding":"hex"}')"
sleep 0.5
EVENTS_RESULT="$(curl -fsS -b "${COOKIE_JAR}" \
  "http://127.0.0.1:${PORT}/api/tcp/sessions/${SESSION_ID}/events?after=0&limit=50")"
curl -fsS -b "${COOKIE_JAR}" -X DELETE "http://127.0.0.1:${PORT}/api/tcp/sessions/${SESSION_ID}" >/dev/null

HTTP_RESULT="${HTTP_RESULT}" PROXY_RESULT="${PROXY_RESULT}" PROXIED="${PROXIED}" \
H2_RESULT="${H2_RESULT}" \
COMP_GZIP="${COMP_GZIP}" COMP_DEFLATE="${COMP_DEFLATE}" COMP_BR="${COMP_BR}" \
COMP_RAW="${COMP_RAW}" \
REDIRECT_HEADERS="${REDIRECT_HEADERS}" REWRITTEN="${REWRITTEN}" \
METHOD_POST="${METHOD_POST}" METHOD_GET="${METHOD_GET}" \
HEADER_HIT="${HEADER_HIT}" HEADER_MISS="${HEADER_MISS}" \
HOST_HIT="${HOST_HIT}" HOST_MISS="${HOST_MISS}" \
ALL_HIT="${ALL_HIT}" ALL_MISS="${ALL_MISS}" \
MULTI_RESULT="${MULTI_RESULT}" MULTI_STATUSES="${MULTI_STATUSES}" \
SESSION_RESULT="${SESSION_RESULT}" SEND_ONE="${SEND_ONE}" SEND_TWO="${SEND_TWO}" \
EVENTS_RESULT="${EVENTS_RESULT}" python3 - <<'PY'
import json
import os
import sys

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


http = json.loads(os.environ["HTTP_RESULT"])
check(http.get("ok") is True, f"HTTP probe failed: {http.get('error')}")
check(http.get("status") == 200, f"HTTP probe status was {http.get('status')}")
check(http.get("streamId") == 0, f"HTTP/1.1 streamId should be 0: {http.get('streamId')}")
check(http.get("retryable") is False, f"HTTP/1.1 retryable should be false: {http.get('retryable')}")

h2_checked = False
if os.environ.get("H2_RESULT"):
    h2 = json.loads(os.environ["H2_RESULT"])
    if h2.get("ok") is not True:
        if "no HTTP/2 support" in (h2.get("error") or ""):
            print("skipping HTTP/2 assertions: SDK built without nghttp2", file=sys.stderr)
        else:
            check(False, f"HTTP/2 probe failed: {h2.get('error')}")
    else:
        h2_checked = True
        check(h2.get("httpVersion") == "HTTP/2",
              f"HTTP/2 version was {h2.get('httpVersion')}")
        check(h2.get("alpn") == "h2", f"HTTP/2 ALPN was {h2.get('alpn')}")
        check(h2.get("streamId", 0) > 0, f"HTTP/2 stream id was {h2.get('streamId')}")
        check(h2.get("bodyText") == "h2-ok", f"HTTP/2 body was {h2.get('bodyText')!r}")
        frames = (h2.get("h2") or {}).get("frames") or []
        types = [frame.get("type") for frame in frames]
        check("HEADERS" in types, f"HTTP/2 frame summary has no HEADERS: {types}")
        check("DATA" in types, f"HTTP/2 frame summary has no DATA: {types}")
        check(h2.get("h2", {}).get("frameCount", 0) >= len(frames),
              "HTTP/2 frame count is smaller than the captured frames")

compression_checked = False
for label, env_name, encoding in (
    ("gzip", "COMP_GZIP", "gzip"),
    ("deflate", "COMP_DEFLATE", "deflate"),
    ("brotli", "COMP_BR", "br"),
):
    raw = os.environ.get(env_name)
    if not raw:
        continue
    decoded = json.loads(raw)
    info = decoded.get("compression") or {}
    compression_checked = True
    check(decoded.get("ok") is True, f"{label} probe failed: {decoded.get('error')}")
    check(decoded.get("bodyText", "").startswith("compressed-ok"),
          f"{label} body was not decoded: {decoded.get('bodyText', '')[:40]!r}")
    check(info.get("decoded") is True, f"{label} was not marked decoded: {info}")
    check(info.get("contentEncoding") == encoding,
          f"{label} contentEncoding was {info.get('contentEncoding')}")
    check(info.get("wireBodySize", 0) > 0 and
          info.get("wireBodySize", 0) < info.get("bodySize", 0),
          f"{label} wire/decoded sizes look wrong: {info}")

if os.environ.get("COMP_RAW"):
    raw_result = json.loads(os.environ["COMP_RAW"])
    check(raw_result.get("ok") is True, f"raw compression probe failed: {raw_result.get('error')}")
    check(raw_result.get("bodyEncoding") == "base64",
          f"raw compressed body should be binary: {raw_result.get('bodyEncoding')}")
    check(raw_result.get("compression") is None,
          "raw compressed response should not claim decompression")
    encodings = [h.get("value") for h in raw_result.get("headers", [])
                 if h.get("name", "").lower() == "content-encoding"]
    check(encodings == ["gzip"], f"raw response lost content-encoding: {encodings}")

proxy = json.loads(os.environ["PROXY_RESULT"])
check(proxy.get("ok") is True, f"proxy creation failed: {proxy.get('error')}")
check(any(p.get("running") for p in proxy.get("proxies", [])), "proxy is not running")
check(any(len(p.get("rules", [])) == 6 for p in proxy.get("proxies", [])),
      f"proxy rules were not persisted: {proxy.get('proxies')}")
redirect = os.environ["REDIRECT_HEADERS"].lower()
check("301" in redirect, f"proxy redirect status missing: {redirect[:120]}")
check("location: /api/health?x=1" in redirect,
      f"proxy redirect lost the query: {redirect[:200]}")
rewritten = json.loads(os.environ["REWRITTEN"])
check(rewritten.get("name") == "uvscope",
      f"proxy rewrite did not reach /api/health: {rewritten}")

def status_of(name):
    code, _, location = os.environ[name].partition("|")
    return code, location

def check_rule(name, expected, message):
    code, location = status_of(name)
    check(code == expected and bool(location),
          f"{message}: status={code} location={location!r}")

def check_miss(name, message):
    code, location = status_of(name)
    check(not location, f"{message}: status={code} location={location!r}")

check_rule("METHOD_POST", "307", "method rule did not fire on POST")
check_miss("METHOD_GET", "method rule fired on GET")
check_rule("HEADER_HIT", "302", "header rule did not fire with the header")
check_miss("HEADER_MISS", "header rule fired without the header")
check_rule("HOST_HIT", "301", "host rule did not fire for proxy.test")
check_miss("HOST_MISS", "host rule fired for the wrong host")
check_rule("ALL_HIT", "308", "combined method+host+header rule did not fire")
check_miss("ALL_MISS", "combined rule fired with a missing header")

multi = json.loads(os.environ["MULTI_RESULT"])
check(multi.get("ok") is True, f"multi-upstream proxy creation failed: {multi}")
multi_proxy = next((p for p in multi.get("proxies", []) if p.get("name") == "smoke-multi"), {})
check(multi_proxy.get("retries") == 1,
      f"retries were not persisted: {multi_proxy.get('retries')}")
check(len(multi_proxy.get("upstreams", [])) == 2,
      f"upstreams were not persisted: {multi_proxy.get('upstreams')}")
statuses = os.environ["MULTI_STATUSES"].split()
check(len(statuses) == 10 and all(code == "200" for code in statuses),
      f"failover did not serve every request: {statuses}")

proxied = json.loads(os.environ["PROXIED"])
check(proxied.get("ok") is True, "proxied health endpoint failed")
check(proxied.get("name") == "uvscope", "proxied response was not uvscope")

session = json.loads(os.environ["SESSION_RESULT"])
check(session.get("ok") is True, f"TCP session creation failed: {session.get('error')}")
check(session.get("session", {}).get("state") in ("connecting", "connected"),
      "TCP session did not start")
check(json.loads(os.environ["SEND_ONE"]).get("ok") is True, "first TCP send was rejected")
check(json.loads(os.environ["SEND_TWO"]).get("ok") is True, "second TCP send was rejected")
events = json.loads(os.environ["EVENTS_RESULT"])
check(events.get("ok") is True, "TCP session events failed")
texts = [item.get("payload", {}).get("text") for item in events.get("events", [])
         if item.get("type") == "recv"]
check(texts[:2] == ["echo:smoke-1", "echo:smoke-2"],
      f"TCP session echo mismatch: {texts}")
check(events.get("session", {}).get("queuedWrites") == 0,
      "TCP session still reports queued writes")

if failures:
    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    sys.exit(1)
print("smoke test OK (setup, login, HTTP probe, reverse proxy, persistent TCP session"
      + (", HTTP/2" if h2_checked else "")
      + (", compression" if compression_checked else "") + ")")
PY

BASE="http://127.0.0.1:${PORT}" COOKIE_JAR="${COOKIE_JAR}" \
COMP_PORT="${COMP_PORT}" COMP_READY="${COMP_READY}" WS_PORT="${WS_PORT}" \
TCP_LISTEN_PORT="${TCP_LISTEN_PORT}" UDP_LISTEN_PORT="${UDP_LISTEN_PORT}" \
SOCKS5_PORT="${SOCKS5_PORT}" SOCKS5_AUTH_PORT="${SOCKS5_AUTH_PORT}" \
SOCKS5_UDP_TARGET_PORT="${SOCKS5_UDP_TARGET_PORT}" TCP_PORT="${TCP_PORT}" python3 - <<'PY'
import base64
import hashlib
import http.cookiejar
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

base = os.environ["BASE"]
jar = http.cookiejar.MozillaCookieJar(os.environ["COOKIE_JAR"])
jar.load(ignore_discard=True, ignore_expires=True)
opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"content-type": "application/json"} if body is not None else {}
    request = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        with opener.open(request, timeout=10) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


def wait_listener(listener_id, state="listening", timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        status, payload = call("GET", f"/api/transport/listeners/{listener_id}")
        if payload.get("listener", {}).get("state") == state:
            return True
        time.sleep(0.1)
    return False


status, created = call("POST", "/api/transport/listeners", {
    "transport": "tcp", "name": "smoke-tcp", "host": "127.0.0.1",
    "port": int(os.environ["TCP_LISTEN_PORT"]), "start": True,
})
check(status == 200 and created.get("ok") is True, f"TCP listener create failed: {created}")
tcp_listener = created.get("listener")
check(bool(tcp_listener), "TCP listener id missing")
if tcp_listener:
    check(wait_listener(tcp_listener), "TCP listener did not start")
    client = socket.create_connection(("127.0.0.1", int(os.environ["TCP_LISTEN_PORT"])), timeout=5)
    client.sendall(b"listener-hello")
    session_id = ""
    for _ in range(50):
        status, sessions = call("GET", f"/api/transport/listeners/{tcp_listener}/sessions")
        if sessions.get("sessions"):
            session_id = sessions["sessions"][0]["id"]
            break
        time.sleep(0.1)
    check(bool(session_id), "TCP listener did not accept a session")
    if session_id:
        status, sent = call("POST", f"/api/transport/listeners/{tcp_listener}/send", {
            "sessionId": session_id, "payload": "listener-reply", "encoding": "text",
        })
        check(status == 200 and sent.get("ok") is True, f"listener send failed: {sent}")
        client.settimeout(5)
        reply = client.recv(4096)
        check(reply == b"listener-reply", f"TCP listener reply mismatch: {reply}")
        status, events = call(
            "GET", f"/api/transport/listeners/{tcp_listener}/sessions/{session_id}/events?after=0")
        texts = [item.get("payload", {}).get("text") for item in events.get("events", [])
                 if item.get("type") == "recv"]
        check("listener-hello" in texts, f"TCP listener did not record the client bytes: {texts}")
    client.close()
    status, deleted = call("DELETE", f"/api/transport/listeners/{tcp_listener}")
    check(status == 200 and deleted.get("ok") is True, f"TCP listener delete failed: {deleted}")

status, created = call("POST", "/api/transport/listeners", {
    "transport": "udp", "name": "smoke-udp", "host": "127.0.0.1",
    "port": int(os.environ["UDP_LISTEN_PORT"]), "start": True,
})
check(status == 200 and created.get("ok") is True, f"UDP listener create failed: {created}")
udp_listener = created.get("listener")
check(bool(udp_listener), "UDP listener id missing")
if udp_listener:
    check(wait_listener(udp_listener), "UDP listener did not start")
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", 0))
    udp.settimeout(5)
    udp.sendto(b"udp-smoke", ("127.0.0.1", int(os.environ["UDP_LISTEN_PORT"])))
    peer = ""
    for _ in range(50):
        status, events = call(
            "GET", f"/api/transport/listeners/{udp_listener}/events?after=0&limit=50")
        datagrams = [item for item in events.get("events", []) if item.get("type") == "datagram"]
        if datagrams:
            peer = datagrams[0].get("peer", "")
            break
        time.sleep(0.1)
    check(bool(peer), "UDP listener did not receive the datagram")
    if peer:
        host, port_text = peer.rsplit(":", 1)
        status, sent = call("POST", f"/api/transport/listeners/{udp_listener}/send", {
            "peerHost": host, "peerPort": int(port_text),
            "payload": "udp-reply", "encoding": "text",
        })
        check(status == 200 and sent.get("ok") is True, f"UDP listener send failed: {sent}")
        reply, _ = udp.recvfrom(4096)
        check(reply == b"udp-reply", f"UDP listener reply mismatch: {reply}")
    udp.close()
    status, deleted = call("DELETE", f"/api/transport/listeners/{udp_listener}")
    check(status == 200 and deleted.get("ok") is True, f"UDP listener delete failed: {deleted}")

def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("connection closed")
        data += chunk
    return data


def socks5_connect(port, target_host, target_port, username=None, password=None):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    if username is None:
        sock.sendall(b"\x05\x01\x00")
        method = recv_exact(sock, 2)
        if method != b"\x05\x00":
            sock.close()
            raise RuntimeError(f"method selection failed: {method.hex()}")
    else:
        sock.sendall(b"\x05\x01\x02")
        method = recv_exact(sock, 2)
        if method != b"\x05\x02":
            sock.close()
            raise RuntimeError(f"auth method not selected: {method.hex()}")
        user = username.encode()
        pw = password.encode()
        sock.sendall(b"\x01" + bytes([len(user)]) + user + bytes([len(pw)]) + pw)
        status = recv_exact(sock, 2)
        if status[1] != 0:
            sock.close()
            raise RuntimeError(f"authentication rejected: {status.hex()}")
    host = target_host.encode()
    sock.sendall(b"\x05\x01\x00\x03" + bytes([len(host)]) + host +
                 target_port.to_bytes(2, "big"))
    reply = recv_exact(sock, 4)
    if reply[1] != 0:
        sock.close()
        raise RuntimeError(f"CONNECT rejected: {reply.hex()}")
    atyp = reply[3]
    if atyp == 1:
        recv_exact(sock, 4)
    elif atyp == 4:
        recv_exact(sock, 16)
    elif atyp == 3:
        recv_exact(sock, recv_exact(sock, 1)[0])
    else:
        sock.close()
        raise RuntimeError(f"unexpected address type: {atyp}")
    recv_exact(sock, 2)
    return sock


def read_reply(sock):
    head = recv_exact(sock, 4)
    atyp = head[3]
    if atyp == 1:
        host = socket.inet_ntoa(recv_exact(sock, 4))
    elif atyp == 4:
        host = socket.inet_ntop(socket.AF_INET6, recv_exact(sock, 16))
    elif atyp == 3:
        host = recv_exact(sock, recv_exact(sock, 1)[0]).decode()
    else:
        raise RuntimeError(f"unexpected address type: {atyp}")
    port = int.from_bytes(recv_exact(sock, 2), "big")
    return head[1], host, port


def socks5_greeting(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(b"\x05\x01\x00")
    if recv_exact(sock, 2) != b"\x05\x00":
        sock.close()
        raise RuntimeError("no-auth was not selected")
    return sock


def create_socks5(port, body):
    status, created = call("POST", "/api/socks5/servers", dict(body, port=port, start=True))
    check(status == 200 and created.get("ok") is True, f"SOCKS5 create failed: {created}")
    return created.get("server", "")


def wait_socks5(server_id, timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        status, payload = call("GET", f"/api/socks5/servers/{server_id}")
        if payload.get("server", {}).get("state") == "listening":
            return True
        time.sleep(0.1)
    return False


socks5_server = create_socks5(int(os.environ["SOCKS5_PORT"]), {"name": "smoke-socks5"})
if socks5_server:
    check(wait_socks5(socks5_server), "SOCKS5 server did not start")
    try:
        tunnel = socks5_connect(int(os.environ["SOCKS5_PORT"]), "127.0.0.1",
                                int(os.environ["TCP_PORT"]))
        tunnel.sendall(b"through-socks5")
        reply = recv_exact(tunnel, len(b"echo:through-socks5"))
        check(reply == b"echo:through-socks5", f"SOCKS5 tunnel reply mismatch: {reply}")
        tunnel.close()
    except (OSError, RuntimeError, EOFError) as error:
        check(False, f"SOCKS5 tunnel failed: {error}")
    status, sessions = call("GET", f"/api/socks5/servers/{socks5_server}/sessions")
    established = [item for item in sessions.get("sessions", [])
                   if item.get("state") in ("established", "closed")]
    check(bool(established), "SOCKS5 server did not record the session")
    if established and established[0].get("bytesAvailable"):
        session_id = established[0]["id"]
        closed = None
        for _ in range(20):
            status, sessions = call("GET", f"/api/socks5/servers/{socks5_server}/sessions")
            closed = next((item for item in sessions.get("sessions", [])
                           if item.get("id") == session_id), None)
            if closed and closed.get("state") == "closed":
                break
            time.sleep(0.1)
        check(closed is not None and closed.get("state") == "closed",
              f"SOCKS5 session did not close: {closed}")
        if closed:
            check(closed.get("bytesClientToTarget") == len("through-socks5"),
                  f"SOCKS5 client bytes mismatch: {closed.get('bytesClientToTarget')}")
            check(closed.get("bytesTargetToClient") == len("echo:through-socks5"),
                  f"SOCKS5 target bytes mismatch: {closed.get('bytesTargetToClient')}")
    curl = subprocess.run(
        ["curl", "-fsS", "--max-time", "5", "--socks5-hostname",
         f"127.0.0.1:{os.environ['SOCKS5_PORT']}", f"{base}/api/health"],
        capture_output=True)
    check(curl.returncode == 0,
          f"curl --socks5-hostname failed: {curl.stderr.decode(errors='replace')}")

    # Outbound client paths must use the proxy and never fall back to direct.
    proxy = {"host": "127.0.0.1", "port": int(os.environ["SOCKS5_PORT"])}
    status, probed = call("POST", "/api/tcp/probe", {
        "host": "127.0.0.1", "port": int(os.environ["TCP_PORT"]),
        "payload": "client-socks5", "connectTimeoutMs": 3000,
        "readTimeoutMs": 2000, "proxy": proxy,
    })
    check(status == 200 and probed.get("ok") is True,
          f"TCP probe through SOCKS5 failed: {probed}")
    if probed.get("ok"):
        check(probed.get("received", {}).get("text") == "echo:client-socks5",
              f"TCP probe through SOCKS5 got {probed.get('received')}")

    status, http_probe = call("POST", "/api/http/send", {
        "url": f"{base}/api/health", "method": "GET", "protocol": "http1",
        "timeoutMs": 5000, "proxy": proxy,
    })
    check(status == 200 and http_probe.get("ok") is True and http_probe.get("status") == 200,
          f"HTTP probe through SOCKS5 failed: {http_probe}")

    status, udp_probe = call("POST", "/api/udp/probe", {
        "host": "127.0.0.1", "port": int(os.environ["SOCKS5_UDP_TARGET_PORT"]),
        "payload": "udp-client-socks5", "timeoutMs": 3000, "proxy": proxy,
    })
    check(status == 200 and udp_probe.get("ok") is True,
          f"UDP probe through SOCKS5 failed: {udp_probe}")
    if udp_probe.get("ok"):
        check(udp_probe.get("received", {}).get("text") == "udp:udp-client-socks5",
              f"UDP probe through SOCKS5 got {udp_probe.get('received')}")

    # BIND: the server listens, a peer connects, and the control connection
    # becomes the data tunnel after the second reply.
    try:
        ctrl = socks5_greeting(int(os.environ["SOCKS5_PORT"]))
        ctrl.sendall(b"\x05\x02\x00\x01" + socket.inet_aton("127.0.0.1") +
                     (0).to_bytes(2, "big"))
        rep, bind_host, bind_port = read_reply(ctrl)
        check(rep == 0, f"BIND request was rejected: {rep}")
        if rep == 0:
            peer = socket.create_connection((bind_host, bind_port), timeout=5)
            rep2, peer_host, peer_port = read_reply(ctrl)
            check(rep2 == 0, f"BIND peer reply was rejected: {rep2}")
            peer.sendall(b"bind-hello")
            echoed = recv_exact(ctrl, len(b"bind-hello"))
            check(echoed == b"bind-hello", f"BIND inbound mismatch: {echoed}")
            ctrl.sendall(b"bind-reply")
            echoed = recv_exact(peer, len(b"bind-reply"))
            check(echoed == b"bind-reply", f"BIND outbound mismatch: {echoed}")
            peer.close()
        ctrl.close()
    except (OSError, RuntimeError, EOFError) as error:
        check(False, f"BIND round-trip failed: {error}")

    # An unsupported command must be rejected, not silently accepted.
    try:
        ctrl = socks5_greeting(int(os.environ["SOCKS5_PORT"]))
        ctrl.sendall(b"\x05\x04\x00\x01" + socket.inet_aton("127.0.0.1") +
                     (80).to_bytes(2, "big"))
        rep, _, _ = read_reply(ctrl)
        check(rep == 7, f"unsupported command returned REP={rep}, expected 7")
        ctrl.close()
    except (OSError, RuntimeError, EOFError) as error:
        check(False, f"unsupported command probe failed: {error}")

    status, unreachable = call("POST", "/api/tcp/probe", {
        "host": "127.0.0.1", "port": int(os.environ["TCP_PORT"]),
        "payload": "x", "connectTimeoutMs": 2000,
        "proxy": {"host": "127.0.0.1", "port": int(os.environ["SOCKS5_PORT"]) + 50},
    })
    check(status == 200 and unreachable.get("ok") is False,
          f"a dead proxy fell back to a direct connection: {unreachable}")
    check(unreachable.get("proxy", {}).get("errorClass") == "proxy_unreachable",
          f"dead proxy was not classified: {unreachable.get('proxy')}")

    close_port = int(os.environ["SOCKS5_UDP_TARGET_PORT"]) + 50
    status, refused = call("POST", "/api/tcp/probe", {
        "host": "127.0.0.1", "port": close_port, "payload": "x",
        "connectTimeoutMs": 3000, "proxy": proxy,
    })
    check(status == 200 and refused.get("ok") is False,
          f"target refusal unexpectedly succeeded: {refused}")
    check(refused.get("proxy", {}).get("errorClass") == "target_rejected",
          f"target refusal was not classified: {refused.get('proxy')}")

    status, deleted = call("DELETE", f"/api/socks5/servers/{socks5_server}")
    check(status == 200 and deleted.get("ok") is True, f"SOCKS5 delete failed: {deleted}")

auth_server = create_socks5(int(os.environ["SOCKS5_AUTH_PORT"]),
                            {"name": "smoke-socks5-auth", "username": "smoke",
                             "password": "smoke-pass"})
if auth_server:
    check(wait_socks5(auth_server), "authenticated SOCKS5 server did not start")
    status, rejected = call("POST", "/api/tcp/probe", {
        "host": "127.0.0.1", "port": int(os.environ["TCP_PORT"]),
        "payload": "x", "connectTimeoutMs": 3000,
        "proxy": {"host": "127.0.0.1", "port": int(os.environ["SOCKS5_AUTH_PORT"]),
                  "username": "smoke", "password": "wrong"},
    })
    check(status == 200 and rejected.get("ok") is False,
          f"wrong proxy password unexpectedly connected: {rejected}")
    check(rejected.get("proxy", {}).get("errorClass") == "auth_failed",
          f"wrong proxy password was not classified: {rejected.get('proxy')}")
    try:
        socks5_connect(int(os.environ["SOCKS5_AUTH_PORT"]), "127.0.0.1",
                       int(os.environ["TCP_PORT"]), "smoke", "wrong")
        check(False, "SOCKS5 accepted a wrong password")
    except RuntimeError as error:
        check("rejected" in str(error), f"unexpected auth failure: {error}")
    try:
        tunnel = socks5_connect(int(os.environ["SOCKS5_AUTH_PORT"]), "127.0.0.1",
                                int(os.environ["TCP_PORT"]), "smoke", "smoke-pass")
        tunnel.sendall(b"auth-socks5")
        reply = recv_exact(tunnel, len(b"echo:auth-socks5"))
        check(reply == b"echo:auth-socks5", f"authenticated tunnel mismatch: {reply}")
        tunnel.close()
    except (OSError, RuntimeError, EOFError) as error:
        check(False, f"authenticated SOCKS5 tunnel failed: {error}")
    status, deleted = call("DELETE", f"/api/socks5/servers/{auth_server}")
    check(status == 200 and deleted.get("ok") is True, f"SOCKS5 auth delete failed: {deleted}")

# Managed certificates: generate, inspect, import the same pair, delete.
status, generated = call("POST", "/api/tls/certificates/generate", {
    "name": "smoke-cert", "commonName": "localhost",
    "sans": ["localhost", "127.0.0.1"], "days": 2,
    "keyType": "ec", "keyBits": 256,
})
cert_meta = generated.get("certificate", {})
check(status == 200 and generated.get("ok") is True,
      f"certificate generation failed: {generated}")
if cert_meta:
    check(os.path.exists(cert_meta.get("certPath", "")), "generated cert.pem is missing")
    key_path = cert_meta.get("keyPath", "")
    check(os.path.exists(key_path), "generated key.pem is missing")
    if key_path and hasattr(os, "stat") and os.name == "posix":
        import stat as stat_module
        check(stat_module.S_IMODE(os.stat(key_path).st_mode) == 0o600,
              "private key permissions are not 0600")
    status, detail = call("GET", f"/api/tls/certificates/{cert_meta['id']}")
    sans = detail.get("certificate", {}).get("certificate", {}).get("san", [])
    check(any(item.get("value") == "127.0.0.1" for item in sans),
          f"generated certificate is missing the IP SAN: {sans}")
    cert_pem = open(cert_meta["certPath"], encoding="utf-8").read()
    key_pem = open(key_path, encoding="utf-8").read()
    status, imported = call("POST", "/api/tls/certificates/import", {
        "name": "smoke-import", "certPem": cert_pem, "keyPem": key_pem,
    })
    check(status == 200 and imported.get("ok") is True,
          f"certificate import failed: {imported}")
    if imported.get("certificate", {}).get("id"):
        status, deleted = call(
            "DELETE", f"/api/tls/certificates/{imported['certificate']['id']}")
        check(status == 200 and deleted.get("ok") is True,
              f"imported certificate delete failed: {deleted}")
    status, deleted = call("DELETE", f"/api/tls/certificates/{cert_meta['id']}")
    check(status == 200 and deleted.get("ok") is True,
          f"generated certificate delete failed: {deleted}")
    check(not os.path.exists(cert_meta["certPath"]), "certificate files were not removed")

# Virtual terminal: the module is off by default, then a real PTY round-trip.
status, settings = call("PUT", "/api/settings", {"modules": {"terminal": True}})
check(status == 200 and settings.get("ok") is True,
      f"could not enable the terminal module: {settings}")
status, catalog = call("GET", "/api/terminal/commands?platform=linux")
check(status == 200 and len(catalog.get("commands", [])) > 50,
      f"terminal command catalog was not seeded: {len(catalog.get('commands', []))}")
check(any(item.get("quick") for item in catalog.get("commands", [])),
      "terminal command catalog has no quick entries")
status, added_command = call("POST", "/api/terminal/commands", {
    "name": "smoke command", "command": "echo smoke-command",
    "category": "custom", "platform": "linux", "description": "smoke",
    "quick": True,
})
check(status == 200 and added_command.get("ok") is True,
      f"terminal command add failed: {added_command}")
command_id = added_command.get("command", {}).get("id")
if command_id:
    status, edited = call("PATCH", f"/api/terminal/commands/{command_id}", {"quick": False})
    check(status == 200 and edited.get("command", {}).get("quick") is False,
          f"terminal command edit failed: {edited}")
    status, removed = call("DELETE", f"/api/terminal/commands/{command_id}")
    check(status == 200 and removed.get("ok") is True,
          f"terminal command delete failed: {removed}")
status, created = call("POST", "/api/terminal/sessions",
                       {"name": "smoke", "cols": 80, "rows": 24})
check(status == 200 and created.get("session", {}).get("state") == "running",
      f"terminal create failed: {created}")
terminal_id = created.get("session", {}).get("id", "")
if terminal_id:
    status, sent = call("POST", f"/api/terminal/sessions/{terminal_id}/input",
                        {"data": "printf 'term-smoke\\n'; exit\n", "encoding": "text"})
    check(status == 200 and sent.get("ok") is True, f"terminal input failed: {sent}")
    terminal_state = ""
    terminal_text = ""
    for _ in range(60):
        status, output = call(
            "GET", f"/api/terminal/sessions/{terminal_id}/output?after=0&limit=200")
        chunks = output.get("chunks", [])
        terminal_text = b"".join(base64.b64decode(item["data"]) for item in chunks).decode(
            errors="replace")
        terminal_state = output.get("session", {}).get("state", "")
        if terminal_state == "exited" and "term-smoke" in terminal_text:
            break
        time.sleep(0.1)
    check(terminal_state == "exited", f"terminal did not exit: {terminal_state}")
    check(output.get("session", {}).get("exitCode") == 0,
          f"terminal exit code: {output.get('session', {}).get('exitCode')}")
    check("term-smoke" in terminal_text, f"terminal output missing: {terminal_text!r}")
    status, deleted = call("DELETE", f"/api/terminal/sessions/{terminal_id}")
    check(status in (200, 400), f"terminal close failed: {deleted}")
status, settings = call("PUT", "/api/settings", {"modules": {"terminal": False}})
check(status == 200 and settings.get("ok") is True,
      f"could not disable the terminal module: {settings}")

# Preview documents: safe mode keeps scripts off, scripts mode opts in, both
# carry a CSP sandbox and a base URL injection.
for mode, csp_must, csp_must_not in [
    ("safe", "script-src 'none'", "allow-scripts"),
    ("scripts", "sandbox allow-scripts", None),
]:
    status, preview = call("POST", "/api/preview", {
        "html": "<!doctype html><html><head></head><body><div id=\"p\">ok</div></body></html>",
        "baseUrl": base + "/target/page",
        "mode": mode,
    })
    check(status == 200 and bool(preview.get("url")),
          f"preview {mode} create failed: {preview}")
    if preview.get("url"):
        with urllib.request.urlopen(base + preview["url"], timeout=5) as response:
            csp = response.headers.get("content-security-policy", "")
            page = response.read().decode()
        check(csp_must in csp, f"preview {mode} CSP missing {csp_must}: {csp}")
        if csp_must_not:
            check(csp_must_not not in csp,
                  f"preview {mode} CSP unexpectedly has {csp_must_not}: {csp}")
        check(f'<base href="{base}/target/page">' in page,
              f"preview {mode} did not inject the base URL")
        status, deleted = call("DELETE", f"/api/preview/{preview['id']}")
        check(status == 200 and deleted.get("ok") is True,
              f"preview {mode} delete failed: {deleted}")

if os.environ.get("COMP_READY") == "1":
    big_size = 6 * 1024 * 1024
    comp_base = "http://127.0.0.1:" + os.environ["COMP_PORT"]
    status, created = call("POST", "/api/http/download", {
        "url": comp_base + "/big.bin", "method": "GET", "headers": [],
        "body": "", "timeoutMs": 30000,
    })
    download = created.get("download", {})
    download_id = download.get("id", "")
    check(status == 200 and bool(download_id),
          f"download start failed: {created}")
    state = download
    for _ in range(150):
        status, payload = call("GET", f"/api/http/download/{download_id}")
        state = payload.get("download", {})
        if state.get("state") != "running":
            break
        time.sleep(0.2)
    check(state.get("state") == "ready", f"download did not finish: {state}")
    check(state.get("bytes") == big_size,
          f"download size was {state.get('bytes')}, expected {big_size}")
    with opener.open(base + f"/download/{download_id}", timeout=30) as stream:
        body = stream.read()
    expected = bytes(range(256)) * (big_size // 256)
    check(len(body) == big_size, f"downloaded {len(body)} bytes, expected {big_size}")
    check(hashlib.sha256(body).digest() == hashlib.sha256(expected).digest(),
          "downloaded bytes differ from the upstream payload")
    status, deleted = call("DELETE", f"/api/http/download/{download_id}")
    check(status == 200 and deleted.get("ok") is True,
          f"download delete failed: {deleted}")
    try:
        opener.open(base + f"/download/{download_id}", timeout=5)
        check(False, "deleted download is still reachable")
    except urllib.error.HTTPError as error:
        check(error.code == 404, f"deleted download returned {error.code}")

if os.environ.get("WS_PORT"):
    ws_url = "ws://127.0.0.1:" + os.environ["WS_PORT"] + "/echo"
    status, created = call("POST", "/api/ws/sessions", {
        "url": ws_url,
        "headers": [{"name": "X-Smoke", "value": "ok"}],
        "timeoutMs": 5000,
    })
    ws_session = created.get("session", {})
    ws_id = ws_session.get("id", "")
    check(status == 200 and bool(ws_id), f"websocket create failed: {created}")
    for _ in range(60):
        _, payload = call("GET", f"/api/ws/sessions/{ws_id}")
        ws_session = payload.get("session", {})
        if ws_session.get("state") != "connecting":
            break
        time.sleep(0.1)
    check(ws_session.get("state") == "open",
          f"websocket did not open: {ws_session}")

    status, sent = call("POST", f"/api/ws/sessions/{ws_id}/send",
                        {"type": "text", "encoding": "text", "payload": "smoke-ws"})
    check(status == 200 and sent.get("ok") is True,
          f"websocket text send failed: {sent}")
    status, pinged = call("POST", f"/api/ws/sessions/{ws_id}/send",
                          {"type": "ping", "encoding": "text", "payload": "pp"})
    check(status == 200 and pinged.get("ok") is True,
          f"websocket ping failed: {pinged}")

    ws_events = []
    for _ in range(60):
        _, payload = call("GET", f"/api/ws/sessions/{ws_id}/events?after=0&limit=200")
        ws_events = payload.get("events", [])
        texts = [item.get("text") for item in ws_events if item.get("type") == "text"]
        pongs = [item for item in ws_events
                 if item.get("type") == "frame" and item.get("opcode") == "PONG"]
        if "echo:smoke-ws" in texts and pongs:
            break
        time.sleep(0.1)
    texts = [item.get("text") for item in ws_events if item.get("type") == "text"]
    pongs = [item for item in ws_events
             if item.get("type") == "frame" and item.get("opcode") == "PONG"]
    check("echo:smoke-ws" in texts, f"websocket echo missing: {texts}")
    check(bool(pongs), "websocket PONG frame missing")
    frames = [item for item in ws_events if item.get("type") == "frame"]
    check(all("opcode" in item and "length" in item for item in frames),
          f"websocket frame events are incomplete: {frames[:2]}")

    status, closed = call("POST", f"/api/ws/sessions/{ws_id}/close",
                          {"code": 1000, "reason": "smoke"})
    check(status == 200 and closed.get("ok") is True,
          f"websocket close failed: {closed}")
    for _ in range(60):
        _, payload = call("GET", f"/api/ws/sessions/{ws_id}")
        ws_session = payload.get("session", {})
        if ws_session.get("state") == "closed":
            break
        time.sleep(0.1)
    check(ws_session.get("state") == "closed",
          f"websocket did not close: {ws_session}")
    check(ws_session.get("closeCode") == 1000,
          f"websocket close code was {ws_session.get('closeCode')}")
    status, deleted = call("DELETE", f"/api/ws/sessions/{ws_id}")
    check(status == 200 and deleted.get("ok") is True,
          f"websocket delete failed: {deleted}")

    # The handshake server requires X-Smoke: ok; without it the Upgrade must
    # fail, proving both the header plumbing and the error path.
    status, denied = call("POST", "/api/ws/sessions", {
        "url": ws_url, "timeoutMs": 3000,
    })
    denied_id = denied.get("session", {}).get("id", "")
    check(status == 200 and bool(denied_id), f"denied create failed: {denied}")
    denied_state = {}
    for _ in range(60):
        _, payload = call("GET", f"/api/ws/sessions/{denied_id}")
        denied_state = payload.get("session", {})
        if denied_state.get("state") not in ("connecting", ""):
            break
        time.sleep(0.1)
    check(denied_state.get("state") == "error",
          f"handshake without the header should fail: {denied_state}")
    call("DELETE", f"/api/ws/sessions/{denied_id}")

status, proxy_payload = call("GET", "/api/proxies")
smoke_proxy = next((item for item in proxy_payload.get("proxies", [])
                    if item.get("name") == "smoke"), {})
metrics = smoke_proxy.get("metrics", {})
check(metrics.get("windowSeconds") == 60,
      f"proxy metrics window missing: {metrics}")
check(metrics.get("qps", 0) > 0, f"proxy qps was not recorded: {metrics}")
check(metrics.get("status2xx", 0) > 0,
      f"proxy 2xx status counts missing: {metrics}")
check(metrics.get("p95Ms", 0) >= metrics.get("p50Ms", 0),
      f"proxy latency percentiles are inverted: {metrics}")

if failures:
    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    sys.exit(1)
print("smoke test OK (listener TCP/UDP, SOCKS5, outbound proxy, preview modes, downloads, WebSocket, proxy metrics)")
PY
