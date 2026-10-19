#!/usr/bin/env bash
# End-to-end smoke test used by CI and release verification.
set -euo pipefail

BIN="${1:-./build/uvscope}"
PORT="${2:-18787}"
PROXY_PORT="${3:-18788}"
TCP_PORT="${4:-18789}"
DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/uvscope-smoke.XXXXXX")"
COOKIE_JAR="${DATA_DIR}/cookies.txt"
LOG="${DATA_DIR}/daemon.log"
PID=""
ECHO_PID=""

cleanup() {
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


while True:
    connection, _ = server.accept()
    threading.Thread(target=handle, args=(connection,), daemon=True).start()
PY
python3 "${DATA_DIR}/echo.py" "${TCP_PORT}" >"${DATA_DIR}/echo.log" 2>&1 &
ECHO_PID=$!
for _ in $(seq 1 80); do
  if grep -q LISTENING "${DATA_DIR}/echo.log" 2>/dev/null; then
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

PROXY_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/proxies" \
  -H 'content-type: application/json' \
  -d "{\"name\":\"smoke\",\"listenHost\":\"127.0.0.1\",\"listenPort\":${PROXY_PORT},\"target\":\"http://127.0.0.1:${PORT}\",\"start\":true}")"

sleep 0.3
PROXIED="$(curl -fsS --max-time 5 "http://127.0.0.1:${PROXY_PORT}/api/health")"

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

proxy = json.loads(os.environ["PROXY_RESULT"])
check(proxy.get("ok") is True, f"proxy creation failed: {proxy.get('error')}")
check(any(p.get("running") for p in proxy.get("proxies", [])), "proxy is not running")

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
print("smoke test OK (setup, login, HTTP probe, reverse proxy, persistent TCP session)")
PY
