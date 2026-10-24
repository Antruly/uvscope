#!/usr/bin/env bash
# HTTP/2 over TLS runtime check used by CI on Linux, macOS and Windows (MSYS2).
#
# Usage: h2-smoke.sh <uvscope-binary> [daemon-port] [h2-port]
#
# Starts a throwaway daemon and a local Node.js h2 server, then asks the
# daemon's HTTP probe for a real ALPN h2 request. Requires curl, openssl, node
# and python3 (or python on systems without the versioned name).
set -euo pipefail

BIN="${1:-./build/uvscope}"
PORT="${2:-18820}"
H2_PORT="${3:-18821}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PY="${PYTHON_BIN:-}"
if [[ -z "${PY}" ]]; then
  if command -v python3 >/dev/null 2>&1; then
    PY=python3
  elif command -v python >/dev/null 2>&1; then
    PY=python
  else
    echo "h2-smoke: python3 (or python) is required" >&2
    exit 1
  fi
fi
OPENSSL="${OPENSSL_BIN:-openssl}"
if ! command -v "${OPENSSL}" >/dev/null 2>&1; then
  echo "h2-smoke: openssl (or OPENSSL_BIN) is required" >&2
  exit 1
fi
for tool in curl node; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "h2-smoke: ${tool} is required" >&2
    exit 1
  fi
done

DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/uvscope-h2.XXXXXX")"
DATA_ARG="${DATA_DIR}"
if command -v cygpath >/dev/null 2>&1; then
  DATA_ARG="$(cygpath -m "${DATA_DIR}")"
fi

H2_PID=""
DAEMON_PID=""
cleanup() {
  if [[ -n "${H2_PID}" ]] && kill -0 "${H2_PID}" 2>/dev/null; then
    kill "${H2_PID}" 2>/dev/null || true
    wait "${H2_PID}" 2>/dev/null || true
  fi
  if [[ -n "${DAEMON_PID}" ]] && kill -0 "${DAEMON_PID}" 2>/dev/null; then
    kill "${DAEMON_PID}" 2>/dev/null || true
    local i=0
    while [[ "${i}" -lt 30 ]]; do
      kill -0 "${DAEMON_PID}" 2>/dev/null || break
      sleep 0.1
      i=$((i + 1))
    done
    kill -9 "${DAEMON_PID}" 2>/dev/null || true
  fi
  rm -rf "${DATA_DIR}"
}
trap cleanup EXIT

"${OPENSSL}" req -x509 -newkey rsa:2048 -nodes \
  -keyout "${DATA_DIR}/key.pem" -out "${DATA_DIR}/cert.pem" \
  -days 2 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1

H2_SERVER="${ROOT}/scripts/h2-test-server.mjs"
if command -v cygpath >/dev/null 2>&1; then
  H2_SERVER="$(cygpath -m "${H2_SERVER}")"
  H2_KEY="$(cygpath -m "${DATA_DIR}/key.pem")"
  H2_CERT="$(cygpath -m "${DATA_DIR}/cert.pem")"
else
  H2_KEY="${DATA_DIR}/key.pem"
  H2_CERT="${DATA_DIR}/cert.pem"
fi
node "${H2_SERVER}" "${H2_KEY}" "${H2_CERT}" "${H2_PORT}" \
  >"${DATA_DIR}/h2.log" 2>&1 &
H2_PID=$!
i=0
while [[ "${i}" -lt 80 ]]; do
  grep -q "H2 LISTENING" "${DATA_DIR}/h2.log" 2>/dev/null && break
  sleep 0.1
  i=$((i + 1))
done
if ! grep -q "H2 LISTENING" "${DATA_DIR}/h2.log" 2>/dev/null; then
  echo "h2-smoke: the local h2 server did not start" >&2
  cat "${DATA_DIR}/h2.log" >&2
  exit 1
fi

UVSCOPE_DATA_DIR="${DATA_ARG}" "${BIN}" --host 127.0.0.1 --port "${PORT}" \
  >"${DATA_DIR}/daemon.log" 2>&1 &
DAEMON_PID=$!
i=0
while [[ "${i}" -lt 100 ]]; do
  if curl -fsS --max-time 1 "http://127.0.0.1:${PORT}/api/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "${DAEMON_PID}" 2>/dev/null; then
    echo "h2-smoke: the daemon exited during startup" >&2
    cat "${DATA_DIR}/daemon.log" >&2
    exit 1
  fi
  sleep 0.1
  i=$((i + 1))
done
if ! curl -fsS --max-time 2 "http://127.0.0.1:${PORT}/api/health" >/dev/null 2>&1; then
  echo "h2-smoke: the daemon did not become ready" >&2
  cat "${DATA_DIR}/daemon.log" >&2
  exit 1
fi

JAR="${DATA_DIR}/cookies.txt"
curl -fsS -c "${JAR}" -X POST "http://127.0.0.1:${PORT}/api/setup/init" \
  -H 'content-type: application/json' \
  -d '{"username":"h2-smoke","password":"h2-smoke-password"}' >/dev/null

RESULT="$(curl -fsS -b "${JAR}" -X POST "http://127.0.0.1:${PORT}/api/http/send" \
  -H 'content-type: application/json' \
  -d "{\"url\":\"https://127.0.0.1:${H2_PORT}/h2\",\"method\":\"GET\",\"protocol\":\"http2\",\"headers\":[],\"body\":\"\",\"timeoutMs\":8000,\"insecure\":true}")"

printf '%s' "${RESULT}" | "${PY}" -c '
import json
import sys

result = json.load(sys.stdin)
if result.get("ok") is not True:
    error = result.get("error") or ""
    if "no HTTP/2 support" in error:
        raise SystemExit("h2-smoke: this libuvcpp build has no HTTP/2 support")
    raise SystemExit("h2-smoke: probe failed: %s" % (error or result,))
version = result.get("httpVersion")
alpn = result.get("alpn")
body = result.get("bodyText")
if version != "HTTP/2":
    raise SystemExit("h2-smoke: version was %r" % (version,))
if alpn != "h2":
    raise SystemExit("h2-smoke: ALPN was %r" % (alpn,))
if body != "h2-ok":
    raise SystemExit("h2-smoke: body was %r" % (body,))
stream_id = int(result.get("streamId", 0))
if stream_id <= 0:
    raise SystemExit("h2-smoke: no stream id")
frames = (result.get("h2") or {}).get("frames", [])
types = {frame.get("type") for frame in frames}
if "HEADERS" not in types or "DATA" not in types:
    raise SystemExit("h2-smoke: frame summary is incomplete: %r" % (sorted(types),))
print("H2 smoke OK: HTTP/2, ALPN h2, stream", stream_id, "frames", len(frames))
'
