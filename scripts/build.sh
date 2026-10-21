#!/usr/bin/env bash
# One-command developer build: libuvcpp 1.8.2 SDK + Vue frontend + uvscope.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

if [[ "$(uname -s)" == "Darwin" && -z "${OPENSSL_ROOT_DIR:-}" ]] && command -v brew >/dev/null 2>&1; then
  export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
  export CMAKE_PREFIX_PATH="${OPENSSL_ROOT_DIR}"
fi

sdk_is_current() {
  local prefix="$1"
  local header="${prefix}/include/uvcpp/uvcpp_version.h"
  [[ -f "${prefix}/include/uvcpp/uvcpp_config.h" && -f "${header}" ]] || return 1
  local major minor patch
  major="$(awk '/^#define UVCPP_VERSION_MAJOR /{print $3; exit}' "${header}")"
  minor="$(awk '/^#define UVCPP_VERSION_MINOR /{print $3; exit}' "${header}")"
  patch="$(awk '/^#define UVCPP_VERSION_PATCH /{print $3; exit}' "${header}")"
  [[ "${major}" =~ ^[0-9]+$ && "${minor}" =~ ^[0-9]+$ && "${patch}" =~ ^[0-9]+$ ]] || return 1
  (( major > 1 || (major == 1 && (minor > 8 || (minor == 8 && patch >= 2))) )) || return 1
  # The managed SDK enables HTTP/2 and brotli by default; a cached 1.8.2 tree built
  # without them must be rebuilt rather than silently dropping the features.
  if [[ "${UVCPP_ENABLE_NGHTTP2:-ON}" == "ON" ]]; then
    grep -q '^#  define UVCPP_NGHTTP2_ENABLE 1$' "${prefix}/include/uvcpp/uvcpp_config.h" || return 1
  fi
  if [[ "${UVCPP_ENABLE_BROTLI:-ON}" == "ON" ]]; then
    grep -q '^#  define UVCPP_BROTLI_ENABLE 1$' "${prefix}/include/uvcpp/uvcpp_config.h" || return 1
  fi
  return 0
}

if [[ -z "${UVCPP_ROOT:-}" ]]; then
  UVCPP_ROOT="${ROOT_DIR}/.deps/libuvcpp"
fi
if [[ "${UVCPP_ROOT}" == "${ROOT_DIR}/.deps/libuvcpp" ]] && ! sdk_is_current "${UVCPP_ROOT}"; then
  "${ROOT_DIR}/scripts/build-libuvcpp.sh" "${ROOT_DIR}/.deps/libuvcpp-src" "${UVCPP_ROOT}"
fi

npm --prefix "${ROOT_DIR}/web" ci --no-audit --no-fund
npm --prefix "${ROOT_DIR}/web" run build

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_ROOT="${UVCPP_ROOT}"
cmake --build "${BUILD_DIR}" -j "${JOBS}"
"${BUILD_DIR}/uvscope" --self-check
echo
echo "Build complete: ${BUILD_DIR}/uvscope"
echo "Run: UVSCOPE_DATA_DIR=~/.local/share/uvscope ${BUILD_DIR}/uvscope --port 8787 --open"
