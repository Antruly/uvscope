#!/usr/bin/env bash
# End-to-end smoke test used by CI and release verification.
set -euo pipefail

BIN="${1:-./build/uvscope}"
PORT="${2:-18787}"
PROXY_PORT="${3:-18788}"
DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/uvscope-smoke.XXXXXX")"
COOKIE_JAR="${DATA_DIR}/cookies.txt"
LOG="${DATA_DIR}/daemon.log"
PID=""

cleanup() {
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

HTTP_RESULT="${HTTP_RESULT}" PROXY_RESULT="${PROXY_RESULT}" PROXIED="${PROXIED}" python3 - <<'PY'
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

if failures:
    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    sys.exit(1)
print("smoke test OK (setup, login, HTTP probe, reverse proxy)")
PY
