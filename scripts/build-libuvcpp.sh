#!/usr/bin/env bash
# Build the libuvcpp release runtime used by uvscope.
#
# uvscope requires libuvcpp 1.8.2 or newer; the fixes uvscope used to carry as
# local patches (TLS SNI, POSIX blocking-I/O timeouts, complete install tree,
# SOCKS5 proxy stage) are part of the released SDK. See
# docs/LIBUVCPP_UPSTREAM.zh-CN.md for the history.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${1:-${ROOT_DIR}/.deps/libuvcpp-src}"
PREFIX="${2:-${ROOT_DIR}/.deps/libuvcpp}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
LIBUVCPP_TAG="${LIBUVCPP_TAG:-v1.8.2}"

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

# A cached tree may sit on an older tag (or carry the historical local patches).
# Move it to the requested release tag instead of failing on a patch mismatch.
if git -C "${SRC_DIR}" rev-parse --git-dir >/dev/null 2>&1; then
  current_tag="$(git -C "${SRC_DIR}" describe --tags --exact-match 2>/dev/null || true)"
  if [[ "${current_tag}" != "${LIBUVCPP_TAG}" ]]; then
    if ! git -C "${SRC_DIR}" rev-parse --verify --quiet "refs/tags/${LIBUVCPP_TAG}" >/dev/null; then
      git -C "${SRC_DIR}" fetch --depth 1 origin "tag" "${LIBUVCPP_TAG}"
    fi
    git -C "${SRC_DIR}" checkout -f "${LIBUVCPP_TAG}"
    echo "Using libuvcpp ${LIBUVCPP_TAG}."
  fi
fi

# Local patches carried on top of the release tag. Each one must apply cleanly
# (or already be present); see docs/LIBUVCPP_UPSTREAM.zh-CN.md.
PATCH_DIR="${ROOT_DIR}/patches/upstream"
shopt -s nullglob
patches=("${PATCH_DIR}"/0*.patch)
shopt -u nullglob
for patch in "${patches[@]}"; do
  name="$(basename "${patch}")"
  if git -C "${SRC_DIR}" apply --check "${patch}" >/dev/null 2>&1; then
    git -C "${SRC_DIR}" apply "${patch}"
    echo "Applied ${name}."
  elif git -C "${SRC_DIR}" apply --reverse --check "${patch}" >/dev/null 2>&1; then
    echo "${name} already present."
  else
    echo "error: ${name} does not apply and is not already present in ${SRC_DIR}." >&2
    echo "       Update the patch or point LIBUVCPP_TAG at the matching release." >&2
    exit 1
  fi
done

# HTTP/2 and brotli ship in the default SDK; set UVCPP_ENABLE_NGHTTP2=OFF or
# UVCPP_ENABLE_BROTLI=OFF for a lean build. HTTP/3/QUIC stay closed: upstream has not
# validated a release path yet.
UVCPP_ENABLE_NGHTTP2="${UVCPP_ENABLE_NGHTTP2:-ON}"
UVCPP_ENABLE_BROTLI="${UVCPP_ENABLE_BROTLI:-ON}"
# Experimental: QUIC/HTTP/3 need mainline OpenSSL >= 3.2 (QUIC API) and are off in
# release builds. Set both to ON and point OPENSSL_ROOT_DIR at a 3.2+ prefix.
UVCPP_ENABLE_QUIC="${UVCPP_ENABLE_QUIC:-OFF}"
UVCPP_ENABLE_HTTP3="${UVCPP_ENABLE_HTTP3:-${UVCPP_ENABLE_QUIC}}"
# A QUIC build needs a different OpenSSL (>= 3.2). Keep its CMake cache separate so
# switching flags never reuses a cached system OpenSSL.
BUILD_DIR="${SRC_DIR}/build-uvscope"
if [[ "${UVCPP_ENABLE_QUIC}" == "ON" ]]; then
  BUILD_DIR="${SRC_DIR}/build-uvscope-quic"
fi
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

cmake -S "${CMAKE_SRC}" -B "${BUILD_DIR}" \
  -G "${CMAKE_GENERATOR:-Ninja}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${CMAKE_PREFIX}" \
  -DUVCPP_BUILD_WEB=ON \
  -DUVCPP_BUILD_WEBAPP=ON \
  -DUVCPP_BUILD_NET=ON \
  -DUVCPP_ENABLE_OPENSSL=ON \
  -DUVCPP_ENABLE_ZLIB=ON \
  -DUVCPP_ENABLE_BROTLI="${UVCPP_ENABLE_BROTLI}" \
  -DUVCPP_BROTLI_FROM_SOURCE=ON \
  -DUVCPP_ENABLE_NGHTTP2="${UVCPP_ENABLE_NGHTTP2}" \
  -DUVCPP_ENABLE_QUIC="${UVCPP_ENABLE_QUIC}" \
  -DUVCPP_ENABLE_HTTP3="${UVCPP_ENABLE_HTTP3}" \
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

cmake --build "${BUILD_DIR}" -j "${JOBS}"
cmake --install "${BUILD_DIR}"

# On MinGW, llhttp is linked as a DLL but upstream's install rules do not
# always place it in the SDK. Carry it beside libuvcpp.dll.
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  find "${BUILD_DIR}" -name 'libllhttp.dll' -type f | while read -r dll; do
    mkdir -p "${PREFIX}/bin"
    cp -f "${dll}" "${PREFIX}/bin/"
    echo "installed $(basename "${dll}")"
  done
fi
echo "libuvcpp installed to ${PREFIX}"
