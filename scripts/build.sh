#!/usr/bin/env bash
# One-command developer build: patched libuvcpp + Vue frontend + uvscope.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

if [[ -z "${UVCPP_ROOT:-}" ]]; then
  UVCPP_ROOT="${ROOT_DIR}/.deps/libuvcpp"
fi
if [[ ! -f "${UVCPP_ROOT}/include/uvcpp/uvcpp_config.h" ]]; then
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
