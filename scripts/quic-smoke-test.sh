#!/usr/bin/env bash
# Experimental QUIC/HTTP3 end-to-end test.
#
# Requires a daemon and h3-test-server built against a QUIC-enabled libuvcpp
# SDK (UVCPP_ENABLE_QUIC=ON, UVCPP_ENABLE_HTTP3=ON, OpenSSL 3.6+). It does not
# touch the public network: the HTTP/3 origin runs locally.
set -euo pipefail

BIN="${1:-./build-quic/uvscope}"
H3_BIN="${2:-./build-quic/h3-test-server}"
PORT="${3:-18990}"
QUIC_PORT="${4:-18991}"
DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/uvscope-quic-smoke.XXXXXX")"
COOKIE_JAR="${DATA_DIR}/cookies.txt"
LOG="${DATA_DIR}/daemon.log"
H3_LOG="${DATA_DIR}/h3.log"
PID=""
H3_PID=""

cleanup() {
  local rc=$?
  if [[ "${rc}" -ne 0 ]]; then
    echo "--- h3 server log ---" >&2
    cat "${H3_LOG}" >&2 2>/dev/null || true
    echo "--- daemon log ---" >&2
    cat "${LOG}" >&2 2>/dev/null || true
  fi
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
  fi
  if [[ -n "${H3_PID}" ]] && kill -0 "${H3_PID}" 2>/dev/null; then
    kill "${H3_PID}" 2>/dev/null || true
    wait "${H3_PID}" 2>/dev/null || true
  fi
  rm -rf "${DATA_DIR}"
}
trap cleanup EXIT

# Local HTTP/3 origin on an ephemeral port.
"${H3_BIN}" --host 127.0.0.1 --port 0 >"${H3_LOG}" 2>&1 &
H3_PID=$!
H3_PORT=""
for _ in $(seq 1 100); do
  H3_PORT="$(awk '/H3 LISTENING/{print $3; exit}' "${H3_LOG}" 2>/dev/null || true)"
  [[ -n "${H3_PORT}" ]] && break
  if ! kill -0 "${H3_PID}" 2>/dev/null; then
    echo "h3 test server exited during startup" >&2
    exit 1
  fi
  sleep 0.1
done
if [[ -z "${H3_PORT}" ]]; then
  echo "h3 test server did not report a listening port" >&2
  exit 1
fi

UVSCOPE_DATA_DIR="${DATA_DIR}/data" "${BIN}" --host 127.0.0.1 --port "${PORT}" \
  >"${LOG}" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
  if curl -fsS --max-time 1 "http://127.0.0.1:${PORT}/api/health" \
    >"${DATA_DIR}/health.json" 2>/dev/null; then
    break
  fi
  if ! kill -0 "${PID}" 2>/dev/null; then
    echo "daemon exited during startup" >&2
    exit 1
  fi
  sleep 0.1
done

curl -fsS -c "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/setup/init" \
  -H 'content-type: application/json' \
  -d '{"username":"quic-admin","password":"quic-admin-password"}' >/dev/null

# Raw QUIC echo through uvscope's own QUIC server.
curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/quic/servers" \
  -H 'content-type: application/json' \
  -d "{\"name\":\"quic-smoke\",\"host\":\"127.0.0.1\",\"port\":${QUIC_PORT},\"alpn\":\"h3\",\"echo\":true,\"start\":true}" \
  >"${DATA_DIR}/server.json"
for _ in $(seq 1 50); do
  if curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/quic/probe" \
    -H 'content-type: application/json' \
    -d "{\"host\":\"127.0.0.1\",\"port\":${QUIC_PORT},\"alpn\":\"h3\",\"payload\":\"quic-smoke\",\"encoding\":\"text\",\"timeoutMs\":5000,\"verify\":false}" \
    >"${DATA_DIR}/quic.json" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

# HTTP/3 request against the local h3 origin.
curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/http/send" \
  -H 'content-type: application/json' \
  -d "{\"url\":\"https://127.0.0.1:${H3_PORT}/\",\"method\":\"GET\",\"protocol\":\"http3\",\"headers\":[],\"body\":\"\",\"timeoutMs\":8000,\"insecure\":true}" \
  >"${DATA_DIR}/h3.json"

python3 - "${DATA_DIR}/health.json" "${DATA_DIR}/quic.json" "${DATA_DIR}/h3.json" <<'PY'
import json
import sys

health = json.load(open(sys.argv[1]))
quic = json.load(open(sys.argv[2]))
h3 = json.load(open(sys.argv[3]))

assert health["capabilities"]["http3"] is True, health
assert health["capabilities"]["quic"] is True, health
assert quic.get("ok") is True, quic
assert quic.get("negotiatedAlpn") == "h3", quic
assert quic.get("replyText") == "quic-smoke", quic
assert h3.get("ok") is True, h3
assert h3.get("status") == 200, h3
assert h3.get("httpVersion") == "HTTP/3", h3
assert h3.get("alpn") == "h3", h3
assert h3.get("bodyText") == "hello-h3", h3
assert isinstance(h3.get("goaway"), dict), h3
print("QUIC smoke test OK (health caps, raw QUIC echo, local HTTP/3 request)")
PY
