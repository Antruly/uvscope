# Building and packaging

## Prerequisites

| Platform | Toolchain | Packages |
| --- | --- | --- |
| Linux | GCC 11+ / Clang 14+, CMake 3.20+, Ninja | `build-essential cmake ninja-build libuv1-dev zlib1g-dev libssl-dev libsqlite3-dev` |
| Windows | MinGW-w64 GCC (MSYS2 MINGW64) | `mingw-w64-x86_64-toolchain`, `cmake`, `ninja`, `openssl`, `libuv`, `sqlite3`, `zlib`, `git` |
| macOS | Xcode CLT, CMake, Ninja | `brew install libuv openssl@3 sqlite` |

Node.js 20+ is required for the frontend.

## The libuvcpp SDK

uvscope requires the libuvcpp **1.8.2** release SDK. Earlier releases lack the fixes uvscope used
to carry as local patches (TLS SNI, POSIX blocking-I/O timeouts, a complete `cmake --install`
tree and the SOCKS5 proxy failure stage); those are all part of 1.8.1/1.8.2. Configure fails
early when the detected SDK is older than 1.8.2.

Build the SDK from the release tag:

```sh
./scripts/build-libuvcpp.sh
```

This clones `v1.8.2` into `.deps/libuvcpp-src`, applies the patches in
[`patches/upstream/`](../patches/upstream/) (currently the WebSocket handshake-header patch),
builds a lean runtime (HTTP/1.1 + HTTP/2, WebSocket, webapp, SSL, SQLite, WSDL, brotli) and
installs it to `.deps/libuvcpp`. A cached source
tree on an older tag is moved to the requested release automatically. Set `LIBUVCPP_TAG=vX.Y.Z` to
pin another release, `UVCPP_ENABLE_NGHTTP2=OFF` to build without HTTP/2, or
`UVCPP_ENABLE_BROTLI=OFF` to build without brotli (the daemon then advertises and decodes only
gzip/deflate; the smoke test skips its brotli assertion).

To use an existing SDK:

```sh
export UVCPP_ROOT=/path/to/libuvcpp
```

The SDK must expose `include/uvcpp/uvcpp_config.h` and `lib/libuvcpp.so` (or `libuvcpp.dll.a` on
Windows).

## Build

```sh
./scripts/build.sh
# or manually:
npm --prefix web ci
npm --prefix web run build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUVCPP_ROOT="$UVCPP_ROOT"
cmake --build build -j
./build/uvscope --self-check
```

`--self-check` does not touch the network. It verifies:

- PBKDF2 hash and verification;
- SQLite migration, setup, login and session lookup;
- a real libuvcpp HTTP server + client round-trip.

## Run

```sh
./build/uvscope --host 127.0.0.1 --port 8787 --open
```

Data directory defaults:

- Linux: `$XDG_DATA_HOME/uvscope` or `~/.local/share/uvscope`
- macOS: `~/Library/Application Support/uvscope`
- Windows: `%APPDATA%\uvscope`

Override with `--data-dir` or `UVSCOPE_DATA_DIR`.

## Linux packages

```sh
cmake --install build --prefix /tmp/uvscope-stage
cpack --config build/CPackConfig.cmake -G DEB -B build/packages
cpack --config build/CPackConfig.cmake -G RPM -B build/packages
```

The CPack install rules include:

- `/usr/bin/uvscope`
- `/usr/share/uvscope/web`
- `/usr/share/applications/uvscope.desktop` (`Icon=uvscope`)
- `/usr/share/icons/hicolor/{16..512}x{...}/apps/uvscope.png` and
  `/usr/share/icons/hicolor/scalable/apps/uvscope.svg`
- `/usr/lib/<arch>/uvscope/libuvcpp.so` (and `libllhttp.so*` when present)

Runtime dependencies: `libc6`, `libstdc++6`, `libssl3`, `libuv1`, `zlib1g`, `libsqlite3-0`.
The RPM equivalent is `openssl-libs`, `libuv`, `zlib`, `sqlite-libs`.

## Windows

Build in an MSYS2 MINGW64 shell. Use the SDK from `scripts/build-libuvcpp.sh` (adapt paths
for MSYS2) and configure with the MinGW generator:

```sh
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_ROOT=/c/uvscope-sdk/libuvcpp \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build-mingw -j
cmake --install build-mingw --prefix stage
```

The install tree must contain:

```text
stage/bin/uvscope.exe
stage/bin/libuvcpp.dll
stage/bin/libssl-3-x64.dll
stage/bin/libcrypto-3-x64.dll
stage/bin/libgcc_s_seh-1.dll
stage/bin/libstdc++-6.dll
stage/bin/libwinpthread-1.dll
stage/web/index.html
```

The CI workflow copies the MinGW runtime DLLs from the MSYS2 prefix. The Inno Setup script is
[`packaging/windows/uvscope.iss`](../packaging/windows/uvscope.iss).

`packaging/windows/uvscope.rc.in` embeds `packaging/icons/uvscope.ico` into `uvscope.exe`, and the
installer uses the same file for `SetupIconFile` and its shortcuts. The `.ico`, `.icns`, PNG sizes
and SVG all derive from the web favicon (`web/public/favicon.svg`).

Build the installer:

```bat
iscc packaging\windows\uvscope.iss
```

## macOS

```sh
brew install libuv openssl@3 sqlite
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_ROOT="$PWD/.deps/libuvcpp" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build -j
```

macOS builds are source-oriented in 0.4.2. The install tree carries `share/uvscope/uvscope.icns`,
but a Finder-visible icon needs a signed/notarized `.app`, which is later work.

## CI

- `.github/workflows/ci.yml` builds the frontend and the C++ daemon on Linux, Windows (MinGW) and
  macOS, runs `--self-check` and a real HTTP smoke test.
- `.github/workflows/release.yml` runs on tags `v*` and publishes:
  - Linux x64 / arm64 tar.gz and deb/rpm where supported;
  - Windows x64 zip and Inno Setup installer;
  - macOS arm64 zip.

## Troubleshooting

**`libllhttp.so.9.2: cannot open shared object file`**  
The SDK's `lib` directory is not on the runtime path. Export it:

```sh
export LD_LIBRARY_PATH="$UVCPP_ROOT/lib:$LD_LIBRARY_PATH"
```

The installed uvscope binary uses `$ORIGIN/../lib/uvscope`, so packages do not need this.

**HTTPS handshake fails**  
The CA bundle is not reachable, or the target presents a certificate that does not validate. Set
`UVSCOPE_CA_FILE=/path/to/cacert.pem` or `SSL_CERT_FILE`.

**Frontend is a 503 page**  
`web/dist/index.html` was not built before install. Run `npm --prefix web run build` and
reinstall, or pass `--web-root web/dist`.
