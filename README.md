# uvscope

**A libuvcpp-powered, local-first multi-protocol web debugging workbench and gateway.**

[![CI](https://github.com/Antruly/uvscope/actions/workflows/ci.yml/badge.svg)](https://github.com/Antruly/uvscope/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

uvscope puts HTTP, WebSocket, TCP, UDP, TLS, SOAP/WSDL and managed reverse proxies behind one
Vue 3 control plane. A single C++ daemon owns the event loop, SQLite store, authentication and
all outbound network traffic; the browser only renders state and sends commands.

![uvscope dashboard](docs/screenshots/dashboard.png)

## Why another web debugger?

Most tools stop at request/response. uvscope is designed around the whole handshake:

- **Protocol workbench** — HTTP methods and headers, WebSocket frames, raw TCP/UDP payloads,
  X.509 certificate decoding and WSDL-driven SOAP calls.
- **Gateway in the same process** — launch HTTP reverse proxies, watch request traces, and stop
  them without editing nginx config or restarting the daemon.
- **Local-first security** — SQLite, server-side sessions, PBKDF2 password hashing, RBAC and a
  first-run administrator flow. No telemetry and no cloud account.
- **One network stack** — every outbound connection is driven by
  [libuvcpp](https://github.com/Antruly/libuvcpp) 1.8.0. One event-driven C++ stack from socket
  to UI.
- **A UI that keeps context** — collapsible navigation, cached multi-tab workspaces and live
  telemetry built with Vue 3, Pinia and Vite.

![HTTP Studio](docs/screenshots/http-studio.png)

## What is in 0.3.0

| Area | Status |
| --- | --- |
| HTTP/1.1 request debugger with headers, body, timing and history | ✅ Stable |
| Response preview: sandboxed HTML, images, audio, video and PDF | ✅ New in 0.2.0 |
| Chinese / English UI with browser-language detection and manual switch | ✅ New in 0.2.0 |
| Dark / light / follow-system theme | ✅ New in 0.2.0 |
| HTTPS with system CA bundle / explicit insecure mode | ✅ Stable |
| WebSocket console (browser-native, text + binary) | ✅ Stable |
| TCP and UDP one-shot probes (text / hex) | ✅ Stable |
| Persistent TCP sessions with repeated sends and event logs | ✅ New in 0.3.0 |
| TCP / UDP server listeners with sessions, datagrams and replies | ✅ New in 0.3.0 |
| SOCKS5 server (CONNECT / BIND / UDP ASSOCIATE) and outbound proxy | ✅ New in 0.3.0 |
| TLS certificate decoder (PEM / DER) | ✅ Stable |
| SOAP 1.1 + WSDL operation discovery | ✅ Stable |
| Managed HTTP reverse proxy with live logs | ✅ Stable |
| SQLite persistence, local auth, roles, first-run setup | ✅ Stable |
| Module toggles, factory reset, update check, about/help | ✅ Stable |
| HTTP/2 and HTTP/3 wire transports | 🧪 Experimental protocol preference (HTTP/1.1 is the reliable path in 0.3.0) |
| Raw QUIC streams and HTTP/3 server | 🗺️ Roadmap |

The UI exposes HTTP/2 and HTTP/3 as protocol preferences so the protocol plumbing can be tested.
They are intentionally labelled experimental: the libuvcpp async TLS/h2/h3 path still needs
upstream validation on every target. Use HTTP/1.1 for production debugging today.

SOCKS5 outbound proxying is mandatory when configured: a failed handshake fails the request rather
than falling back to a direct connection.

## Screens

| Gateway | TLS Inspector |
| --- | --- |
| ![reverse proxy](docs/screenshots/reverse-proxy.png) | ![login](docs/screenshots/login.png) |

## Download

Prebuilt **v0.2.0** assets are on the [Releases page](https://github.com/Antruly/uvscope/releases/tag/v0.2.0):

- Linux x64 / arm64: `.tar.gz`, `.deb`, `.rpm`
- Windows x64 (MinGW): `.zip` and `uvscope-0.2.0-setup.exe`
- macOS arm64: `.tar.gz` (Homebrew `libuv` and `openssl@3` are required)

## Quick start

### Requirements

- C++17 compiler, CMake ≥ 3.20, Ninja
- Node.js 20+ and npm
- libuv, zlib, SQLite and OpenSSL development headers
- Linux, macOS, or Windows with MinGW-w64

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build libuv1-dev zlib1g-dev \
  libssl-dev libsqlite3-dev git curl
```

### Build from source

```sh
git clone https://github.com/Antruly/uvscope.git
cd uvscope
./scripts/build.sh
./build/uvscope --port 8787 --open
```

`build.sh` clones libuvcpp v1.8.0 into `.deps/`, applies the upstream fixes from
[`patches/upstream/`](patches/upstream/), builds the frontend, then builds uvscope.

If you already have a patched libuvcpp SDK:

```sh
npm --prefix web ci && npm --prefix web run build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_ROOT=/path/to/libuvcpp
cmake --build build -j
./build/uvscope --self-check
```

Open <http://127.0.0.1:8787>. The first visit creates the administrator account. All runtime
state lives in the data directory printed at startup (`~/.local/share/uvscope` on Linux by
default).

### CLI

```text
uvscope [options]

  --host <addr>       Listen address (default 127.0.0.1)
  --port <port>       Listen port (default 8787)
  --data-dir <path>   SQLite and runtime data
  --web-root <path>   Built frontend directory
  --tls-cert <pem>    Serve the workbench UI over HTTPS
  --tls-key <pem>     Private key paired with --tls-cert
  --open              Open the workbench in the default browser
  --self-check        Run offline smoke tests and exit
  --version           Print version and exit
```

## Architecture

```text
Vue 3 SPA (multi-tab, cached workspaces)
        │  same-origin HTTP + cookies
        ▼
uvscope daemon (C++17 / libuvcpp)
  ├─ auth + RBAC + SQLite (single writer)
  ├─ HTTP API and static asset server
  ├─ protocol probes (HTTP/TCP/UDP/TLS)
  └─ reverse proxy runtimes (one libuv loop each)
        │
        └─ libuvcpp → libuv → sockets
```

The daemon is the only process that opens outbound sockets. The browser never talks to the
target service directly, except for the WebSocket console, where browser-native WebSocket
semantics (origin, mixed content, certificate prompts) are part of what is being debugged.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the data model and request flow.

## Security notes

- Default bind is `127.0.0.1`. Do not expose uvscope to an untrusted network.
- Passwords are PBKDF2-HMAC-SHA256 (210,000 iterations); sessions are server-side and stored as
  SHA-256 token hashes. Cookies are `HttpOnly` + `SameSite=Lax`.
- The reverse proxy and HTTP Studio can reach internal services. Treat access to uvscope as
  equivalent to code execution on the host network.
- Use the TLS Inspector and `Allow insecure TLS` only on systems you control.
- Report vulnerabilities privately as described in [`SECURITY.md`](SECURITY.md).

## Documentation

- [Development plan (中文)](docs/DEVELOPMENT_PLAN.zh-CN.md)
- [Architecture](docs/ARCHITECTURE.md)
- [HTTP API](docs/API.md)
- [Building and packaging](docs/BUILDING.md)
- [Roadmap](ROADMAP.md)
- [Agent guide](AGENTS.md)
- [Changelog](CHANGELOG.md)

## License

MIT. See [LICENSE](LICENSE).

If uvscope saves you a debugging session, a star on
[GitHub](https://github.com/Antruly/uvscope) helps other protocol wranglers find it.
