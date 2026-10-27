#!/usr/bin/env bash
# Built-in TLS listener regression check.
#
# The embedded TLS server serves the workbench over HTTP/2. This script starts
# a throwaway daemon with a self-signed certificate and checks that:
#   1. a normal client (which offers h2) negotiates HTTP/2 and gets 200;
#   2. ALPN selects h2;
#   3. an HTTP/1.1-only client still gets 200 through the fallback path.
#
# Header names on the h2 path must be lowercase; uvscope normalizes its own
# response headers. This test catches a regression where a custom header makes
# the h2 stream hang.
#
# Usage: tls-smoke.sh <uvscope-binary> [port]
set -euo pipefail

BIN="${1:-./build/uvscope}"
PORT="${2:-18830}"
OPENSSL="${OPENSSL_BIN:-openssl}"

for tool in curl "${OPENSSL}"; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "tls-smoke: ${tool} is required" >&2
    exit 1
  fi
done

DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/uvscope-tls.XXXXXX")"
DATA_ARG="${DATA_DIR}"
if command -v cygpath >/dev/null 2>&1; then
  DATA_ARG="$(cygpath -m "${DATA_DIR}")"
fi

DAEMON_PID=""
cleanup() {
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

UVSCOPE_DATA_DIR="${DATA_ARG}" "${BIN}" --host 127.0.0.1 --port "${PORT}" \
  --tls-cert "${DATA_DIR}/cert.pem" --tls-key "${DATA_DIR}/key.pem" \
  >"${DATA_DIR}/daemon.log" 2>&1 &
DAEMON_PID=$!

i=0
while [[ "${i}" -lt 100 ]]; do
  if curl -sk --max-time 1 "https://127.0.0.1:${PORT}/api/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "${DAEMON_PID}" 2>/dev/null; then
    echo "tls-smoke: the daemon exited during startup" >&2
    cat "${DATA_DIR}/daemon.log" >&2
    exit 1
  fi
  sleep 0.1
  i=$((i + 1))
done

CODE="$(curl -sk --max-time 8 -o /dev/null -w '%{http_code}' \
  "https://127.0.0.1:${PORT}/api/health")"
VERSION="$(curl -sk --max-time 8 -o /dev/null -w '%{http_version}' \
  "https://127.0.0.1:${PORT}/api/health")"
if [[ "${CODE}" != "200" || "${VERSION}" != "2" ]]; then
  echo "tls-smoke: default client got HTTP ${CODE} over HTTP/${VERSION}" >&2
  tail -20 "${DATA_DIR}/daemon.log" >&2
  exit 1
fi

FALLBACK_CODE="$(curl -sk --http1.1 --max-time 8 -o /dev/null -w '%{http_code}' \
  "https://127.0.0.1:${PORT}/api/health")"
FALLBACK_VERSION="$(curl -sk --http1.1 --max-time 8 -o /dev/null -w '%{http_version}' \
  "https://127.0.0.1:${PORT}/api/health")"
if [[ "${FALLBACK_CODE}" != "200" || "${FALLBACK_VERSION}" != "1.1" ]]; then
  echo "tls-smoke: --http1.1 client got HTTP ${FALLBACK_CODE} over HTTP/${FALLBACK_VERSION}" >&2
  tail -20 "${DATA_DIR}/daemon.log" >&2
  exit 1
fi

ALPN="$(echo | timeout 8 "${OPENSSL}" s_client -connect "127.0.0.1:${PORT}" \
  -servername localhost -alpn h2,http/1.1 2>/dev/null | grep -i "ALPN protocol" | head -1 || true)"
case "${ALPN}" in
  *h2*) ;;
  *)
    echo "tls-smoke: ALPN did not select h2: ${ALPN:-<none>}" >&2
    exit 1
    ;;
esac

echo "TLS smoke OK: HTTP/2, ALPN h2, health 200 (HTTP/1.1 fallback 200)"
