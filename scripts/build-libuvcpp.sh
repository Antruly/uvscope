#!/usr/bin/env bash
# Build the patched libuvcpp runtime used by uvscope.
#
# The only source change is TLS SNI support (patches/libuvcpp-1.8.0-sni.patch).
# Without SNI, HTTPS requests to modern virtual hosts fail during the handshake.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${1:-${ROOT_DIR}/.deps/libuvcpp-src}"
PREFIX="${2:-${ROOT_DIR}/.deps/libuvcpp}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
LIBUVCPP_TAG="${LIBUVCPP_TAG:-v1.8.0}"
PATCH_FILE="${ROOT_DIR}/patches/libuvcpp-1.8.0-sni.patch"

mkdir -p "$(dirname "${PREFIX}")"
PREFIX="$(cd "$(dirname "${PREFIX}")" && pwd)/$(basename "${PREFIX}")"

if command -v cygpath >/dev/null 2>&1; then
  CMAKE_SRC="$(cygpath -m "${SRC_DIR}")"
  CMAKE_PREFIX="$(cygpath -m "${PREFIX}")"
else
  CMAKE_SRC="${SRC_DIR}"
  CMAKE_PREFIX="${PREFIX}"
fi

if [[ ! -f "${SRC_DIR}/CMakeLists.txt" ]]; then
  mkdir -p "$(dirname "${SRC_DIR}")"
  git -c core.autocrlf=false clone --depth 1 --branch "${LIBUVCPP_TAG}" \
    https://github.com/Antruly/libuvcpp.git "${SRC_DIR}"
fi

git -C "${SRC_DIR}" config core.autocrlf false >/dev/null 2>&1 || true

if git -C "${SRC_DIR}" apply --check "${PATCH_FILE}" >/dev/null 2>&1; then
  git -C "${SRC_DIR}" apply "${PATCH_FILE}"
  echo "Applied TLS SNI patch."
elif git -C "${SRC_DIR}" apply --reverse --check "${PATCH_FILE}" >/dev/null 2>&1; then
  echo "TLS SNI patch already present."
else
  echo "warning: could not apply or detect the SNI patch; building as-is" >&2
fi

# v0.1.0 uses reliable HTTP/1.1 transport. The HTTP/2 and HTTP/3 preferences in
# the UI remain experimental; enable their libuvcpp switches once upstream async
# TLS/h2/h3 paths are validated on every target.
extra_cmake=()
if [[ "$(uname -s)" == "Darwin" ]]; then
  extra_cmake+=("-DCMAKE_INSTALL_RPATH=@loader_path")
else
  extra_cmake+=('-DCMAKE_INSTALL_RPATH=$ORIGIN')
fi
if [[ -n "${OPENSSL_ROOT_DIR:-}" ]]; then
  if command -v cygpath >/dev/null 2>&1; then
    extra_cmake+=("-DOPENSSL_ROOT_DIR=$(cygpath -m "${OPENSSL_ROOT_DIR}")")
  else
    extra_cmake+=("-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}")
  fi
fi
if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
  export CMAKE_PREFIX_PATH
fi

cmake -S "${CMAKE_SRC}" -B "${CMAKE_SRC}/build-uvscope" \
  -G "${CMAKE_GENERATOR:-Ninja}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${CMAKE_PREFIX}" \
  -DUVCPP_BUILD_WEB=ON \
  -DUVCPP_BUILD_WEBAPP=ON \
  -DUVCPP_BUILD_NET=ON \
  -DUVCPP_ENABLE_OPENSSL=ON \
  -DUVCPP_ENABLE_ZLIB=ON \
  -DUVCPP_ENABLE_BROTLI=OFF \
  -DUVCPP_ENABLE_NGHTTP2=OFF \
  -DUVCPP_ENABLE_QUIC=OFF \
  -DUVCPP_ENABLE_HTTP3=OFF \
  -DUVCPP_ENABLE_WSDL=ON \
  -DUVCPP_ENABLE_DB=ON \
  -DUVCPP_ENABLE_DB_SQLITE=ON \
  -DUVCPP_ENABLE_DB_MYSQL=OFF \
  -DUVCPP_ENABLE_DB_PGSQL=OFF \
  -DUVCPP_ENABLE_DB_ODBC=OFF \
  -DUVCPP_ENABLE_DB_MONGODB=OFF \
  -DUVCPP_ENABLE_REDIS=OFF \
  -DUVCPP_ENABLE_CAPI=OFF \
  -DUVCPP_ENABLE_YYJSON=OFF \
  -DUVCPP_BUILD_TESTS=OFF \
  -DUVCPP_BUILD_BENCH=OFF \
  -DUVCPP_BUILD_EXAMPLES=OFF \
  -DUVCPP_BUILD_EXPAND=OFF \
  -DUVCPP_BUILD_SHARED=ON \
  -DUVCPP_BUILD_STATIC=OFF \
  "${extra_cmake[@]}"

cmake --build "${CMAKE_SRC}/build-uvscope" -j "${JOBS}"
cmake --install "${CMAKE_SRC}/build-uvscope"
echo "libuvcpp installed to ${PREFIX}"
