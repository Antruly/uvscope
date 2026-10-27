# uvscope

**A local-first, multi-protocol web debugging workbench and gateway.**

[![CI](https://github.com/Antruly/uvscope/actions/workflows/ci.yml/badge.svg)](https://github.com/Antruly/uvscope/actions/workflows/ci.yml)
[![Docs](https://github.com/Antruly/uvscope/actions/workflows/docs.yml/badge.svg)](https://antruly.github.io/uvscope/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

uvscope puts HTTP, WebSocket, TCP, UDP, TLS, SOAP/WSDL and managed reverse proxies behind one
Vue 3 control plane. A single C++ daemon owns the event loop, SQLite store, authentication and all
outbound network traffic; long-lived services can move to supervised child processes, and the
browser only renders state and sends commands.

![uvscope dashboard](docs/screenshots/dashboard.png)

## Why uvscope

Most tools stop at request/response. uvscope is built around the whole handshake:

- **One workbench for every protocol** — HTTP/1.1 and HTTP/2, WebSocket, raw TCP/UDP, X.509 and
  WSDL-driven SOAP, with the same history, telemetry and export model.
- **A real gateway, not a mock** — run reverse proxies with routing rules, weighted upstreams,
  retries, health checks, circuit breaking and WebSocket pass-through, then watch the traffic live.
- **Long-lived services without a second stack** — TCP/UDP listeners, SOCKS5 servers, reverse
  proxies and terminal PTYs can run in child processes, all under one desired-state supervisor.
- **Local-first** — SQLite, server-side sessions, PBKDF2 password hashing, RBAC and a first-run
  administrator flow. No telemetry, no cloud account, no external database.
- **One network stack** — every target connection is driven by
  [libuvcpp](https://github.com/Antruly/libuvcpp) 1.8.2, from socket to UI.
- **A UI that keeps context** — collapsible navigation, cached multi-tab workspaces, live
  telemetry and an offline command library built with Vue 3, Pinia and Vite.

## Feature overview

### HTTP and APIs

| Capability | Status |
| --- | --- |
| HTTP/1.1 + HTTPS debugger with headers, body, timing and history | Stable |
| Body modes: raw, form-urlencoded, multipart with files | Stable |
| cURL / HAR import and export | Stable |
| Environments with `{{variables}}`, saved collections, OpenAPI/Postman import | Stable |
| Pre-request / post-response scripts with assertions | Stable (browser-side) |
| HTTP/2 over TLS with ALPN, stream IDs, GOAWAY and frame summary | Stable |
| gzip / deflate / brotli decoding with a raw-body toggle | Stable |
| Large-response streaming download with progress, cancel and Range resume | Stable |
| Response preview: sandboxed HTML, images, audio, video and PDF | Stable (scripts opt-in) |
| SOAP 1.1 + WSDL operation discovery | Stable |
| HTTP/3 and raw QUIC probes | Experimental — client-side; browser h3 needs an h3-capable edge |

### Realtime and transports

| Capability | Status |
| --- | --- |
| WebSocket console: daemon-side backend sessions and browser-native mode | Stable |
| Backend WebSocket headers, subprotocols, frames, ping/pong, close codes | Stable |
| One-shot TCP and UDP probes (text / hex) | Stable |
| Persistent TCP sessions with repeated sends and event logs | Stable |
| TCP / UDP server listeners with sessions, datagrams and replies | Stable |
| SOCKS5 client for HTTP/TCP/UDP with authentication and error classification | Stable |
| SOCKS5 server (CONNECT / BIND / UDP ASSOCIATE) with authentication and session logs | Stable |
| Virtual terminal (Unix PTY / Windows ConPTY) with an offline command library | Stable — off by default |

### Gateway and long-lived services

| Capability | Status |
| --- | --- |
| Reverse proxy: exact / prefix / regex rules, redirects and rewrites | Stable |
| Header / method / host conditions, weighted upstreams, retries | Stable |
| Active upstream health checks, per-upstream circuit breaker, QPS/latency metrics | Stable |
| Proxy config hot reload with runtime config versions | Stable |
| WebSocket pass-through in the reverse proxy | Stable |
| Child-process isolation for proxies, listeners, SOCKS5 servers and terminals | Stable |
| Unified desired-state supervisor: restart, backoff, `restarts`/events in `/api/supervisor` | Stable |
| Managed TLS certificates, local CA, CA-signed certs and in-place renewal | Stable |

### Operations and security

| Capability | Status |
| --- | --- |
| SQLite persistence, first-run setup, local auth, RBAC (admin / operator / viewer) | Stable |
| Server-side sessions, active-session revocation, audit log | Stable |
| Backup download and staged restore | Stable |
| Module toggles, factory reset, update check, about/help | Stable |
| Chinese / English UI with browser-language detection and manual switch | Stable |
| Dark / light / follow-system theme | Stable |
| Linux tar/deb/rpm, Windows zip + installer, macOS tar.gz | Stable |

## Download

Prebuilt **v1.0.0** assets are on the
[Releases page](https://github.com/Antruly/uvscope/releases/tag/v1.0.0). The release ships with
`SHA256SUMS`, a Sigstore keyless signature covering the checksums and a generated SPDX SBOM.

- Linux x64 / arm64: `.tar.gz`, `.deb`, `.rpm`
- Windows x64 (MinGW): `.zip` and `uvscope-1.0.0-setup.exe`
- macOS arm64: `.tar.gz` (Homebrew `libuv` and `openssl@3` are required)

Verify a download with `sha256sum -c SHA256SUMS` and
`cosign verify-blob --bundle SHA256SUMS.sigstore.json --certificate-identity-regexp 'https://github.com/Antruly/uvscope/.*' --certificate-oidc-issuer https://token.actions.githubusercontent.com SHA256SUMS`.

## Quick start

### Requirements

- C++17 compiler, CMake ≥ 3.20, Ninja
- Node.js 20+ and npm
- libuv, zlib, SQLite and OpenSSL development headers
- Linux, macOS, or Windows with MSYS2 MinGW-w64

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

`build.sh` clones the libuvcpp v1.8.2 release into `.deps/`, applies the one minimal upstream
patch shipped in `patches/upstream/` (WebSocket handshake headers), builds the frontend, then
builds uvscope and runs `--self-check`.

If you already have a libuvcpp 1.8.2 SDK:

```sh
npm --prefix web ci && npm --prefix web run build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_ROOT=/path/to/libuvcpp
cmake --build build -j
./build/uvscope --self-check
```

Open <http://127.0.0.1:8787>. The first visit creates the administrator account. All runtime
state lives in the data directory printed at startup (`~/.local/share/uvscope` on Linux by
default; override it with `--data-dir`).

### CLI

```text
uvscope [options]

  --host <addr>       Listen address (default 127.0.0.1)
  --port <port>       Listen port (default 8787, 0 picks a free port)
  --data-dir <path>   SQLite and runtime data
  --web-root <path>   Built frontend directory
  --tls-cert <pem>    Serve the workbench UI over HTTPS
  --tls-key <pem>     Private key paired with --tls-cert
  --open              Open the workbench in the default browser
  --self-check        Run offline smoke tests and exit
  --version           Print version and exit
```

Hidden child modes (`--proxy-child`, `--listener-child`, `--socks5-child`, `--terminal-child`,
`--ipc-child`) are spawned by the daemon and are not meant to be invoked by hand.

## Isolation and supervision

Proxies, TCP/UDP listeners, SOCKS5 servers and virtual terminals accept `isolated: true`. The
parent process keeps the API state, scrollback and ownership metadata; the service runs in a
child process and talks to the parent over a length-prefixed JSON IPC channel.

All four service kinds are registered with one desired-state supervisor:

- a crashed child is restarted with exponential backoff (2–60 s) and a new PID;
- restart counts and events are visible in `/api/supervisor` and `/api/supervisor/events`;
- stopping a service clears its desired state, so the watchdog leaves it down;
- deleting a service unregisters it before teardown;
- a normal terminal `exit` is **not** a crash — the child stays alive so scrollback and the exit
  code remain readable. A crashed terminal is respawned with a fresh shell, and attached
  WebSocket clients receive a `reset` frame before the new output.

See [Service supervisor](docs/API.md#service-supervisor) for the API and
[Architecture](docs/ARCHITECTURE.md#process-model) for the process model.

## Architecture

```text
Vue 3 SPA (multi-tab, cached workspaces)
        │ same-origin HTTP + WebSocket + cookies
        ▼
uvscope daemon (C++17 / libuvcpp)
  ├─ auth + RBAC + SQLite + audit + backup
  ├─ HTTP API and static asset server
  ├─ probes, debug sessions and terminal sessions
  ├─ desired-state supervisor
  └─ optional child processes (isolated services)
        ├─ reverse proxy runtime
        ├─ TCP/UDP listener runtime
        ├─ SOCKS5 server runtime
        └─ terminal PTY / ConPTY
              │
              └─ libuvcpp → libuv → sockets
```

The daemon is the only process that serves the UI and owns SQLite. In the default in-process
mode it also owns every outbound socket; with isolation enabled, a service opens its own sockets
inside its child process. The WebSocket view additionally offers a browser-native mode because
origin, mixed-content and certificate prompts are part of what is being debugged.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the data model and request flows.

## Supported platforms

| Platform | Release | Notes |
| --- | --- | --- |
| Linux x64 | tar.gz, deb, rpm | Primary CI and smoke matrix |
| Linux arm64 | tar.gz, deb, rpm | Release build and self-check |
| Windows x64 (MinGW) | zip, setup.exe | CI build, self-check and ConPTY smoke |
| macOS arm64 | tar.gz | CI build, self-check and HTTP/2 runtime check |
| Windows arm64 | — | Post-1.0 (MSYS2 CLANGARM64 is still preview) |
| HTTP/3 / QUIC | Experimental | Requires an OpenSSL 3.2+ SDK build; closed in release builds |

## Security model

- Default bind is `127.0.0.1`. Do not expose uvscope to an untrusted network without an
  authenticating proxy.
- Passwords are PBKDF2-HMAC-SHA256 (210,000 iterations); sessions are server-side and stored as
  SHA-256 token hashes. Cookies are `HttpOnly` + `SameSite=Lax`.
- Anyone who can authenticate can reach the host network, start listeners/proxies, and — when the
  terminal module is enabled — obtain a shell as the daemon user. Treat access as equivalent to
  code execution on that host.
- Isolated services run as the same OS user; isolation is a fault boundary, not a security
  sandbox.
- Use the TLS Inspector and “Allow insecure TLS” only against systems you control.
- Report vulnerabilities privately as described in [SECURITY.md](SECURITY.md).

## Documentation

- **Documentation site**: <https://antruly.github.io/uvscope/>
- [Architecture](docs/ARCHITECTURE.md)
- [HTTP API](docs/API.md)
- [Event schema](docs/EVENTS.md)
- [Extending uvscope](docs/EXTENDING.md)
- [Performance baselines](docs/PERFORMANCE.md)
- [Operations](docs/OPERATIONS.md)
- [Building and packaging](docs/BUILDING.md)
- [Roadmap](ROADMAP.md)
- [Changelog](CHANGELOG.md)
- [Development plan (中文)](docs/DEVELOPMENT_PLAN.zh-CN.md)
- [Agent guide](AGENTS.md)

## Development

```sh
npm --prefix web ci
npm --prefix web run build           # typecheck + bundle
./scripts/build.sh                   # SDK, frontend, daemon, self-check
bash ./scripts/smoke-test.sh ./build/uvscope 18787 18788
./scripts/h2-smoke.sh ./build/uvscope 18820 18821
./scripts/bench.sh ./build/uvscope
mkdocs serve                         # documentation site (pip install mkdocs-material)
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the workflow and verification rules.

## License

MIT. See [LICENSE](LICENSE).

If uvscope saves you a debugging session, a star on
[GitHub](https://github.com/Antruly/uvscope) helps other protocol wranglers find it.
