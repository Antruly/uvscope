# Changelog

All notable changes to uvscope are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this project adheres to
[Semantic Versioning](https://semver.org/).

## [0.9.0] - 2026-10-24

### Added

- Reverse proxies accept per-instance `connectTimeoutMs` (100–120000, default 15000),
  `requestTimeoutMs` (100–300000, default 30000) and `workers` (1–16, default 4). The values
  persist in SQLite with an automatic migration for existing databases, apply to HTTP upstreams
  and WebSocket upgrades, and are editable in the proxy editor. The smoke test covers
  persistence and a 500 ms request timeout returning a structured 502.
- Reverse-proxy configuration is hot-reloaded for name, target/upstreams, retries, upstream
  timeouts and routing rules. The listener and worker pool stay up, `configVersion` increments,
  in-flight requests keep their snapshot, and existing WebSocket sessions keep their upstream.
  Listener, TLS certificate and worker-count changes still stop and restart the runtime. The
  smoke test verifies the hot path (unchanged `startedAt`, new rule active, `hot-reloaded` log)
  and the restart path for a worker-count change.
- Per-upstream circuit breaker. `breakerThreshold` (0–100, default 5; 0 disables) consecutive
  transport failures or 5xx responses open an upstream circuit for `breakerCooldownMs`
  (1000–300000, default 30000), after which one half-open probe is allowed. A successful probe
  closes the circuit; while open the upstream is skipped. `health[].breaker` exposes
  `open`/`failures`/`retryAfterMs`/`lastError`, open and close events are logged, and a request
  with every upstream circuit-open returns `502` without contacting them. The breaker settings
  are hot-reloadable and editable in the proxy editor. The smoke test drives a flaky origin
  through open, fast-fail and recovery.

### Changed

- Reverse-proxy upstream requests now run on a per-proxy worker pool (default 4 threads,
  configurable; 32 queued requests) instead of blocking the proxy's libuv loop. A slow upstream no longer prevents other
  requests on the same proxy from being served; a full queue returns `503` with
  `proxy is stopping or overloaded`. Stopping a proxy joins its workers and drains posted
  completions before closing the listener. Retries, health-filtered upstream selection, logging
  and metrics are unchanged. The smoke test sends a 2 s request and a fast request through one
  proxy and asserts the fast one completes in under a second.

## [0.8.1] - 2026-10-24

### Added

- HTTP/3 probe results now include the peer GOAWAY state on QUIC-enabled builds:
  `goaway: {received, lastStreamId}` alongside `alpn`, `streamId` and `retryable`. libuvcpp does
  not expose an H3 GOAWAY error code, so `errorCode` is omitted rather than fabricated. The
  HTTP Studio timing panel accepts the missing error code. QPACK summaries, 0-RTT and migration
  events remain unavailable.
- Experimental QUIC/HTTP3 CI: the `QUIC experimental` workflow builds OpenSSL 3.6.5 and a
  QUIC-enabled libuvcpp SDK on Linux, compiles the local `scripts/h3-test-server.cpp` origin,
  and runs `scripts/quic-smoke-test.sh`. The test asserts the `/api/health` capabilities, a raw
  QUIC echo probe and an HTTP/3 request (`200`, `HTTP/3`, ALPN `h3`, body `hello-h3`) against the
  local origin. It runs weekly, on demand, and when QUIC-related files change.

### Fixed

- Windows MinGW builds no longer fail in `certificate_common_name`: `<wincred.h>` (pulled
  in by `uv.h`) defines `X509_NAME` as a credential type, so the function now uses the
  `struct X509_name_st` tag like the rest of the certificate code. This unblocks the Windows
  CI job and release builds.
- The experimental QUIC SDK build now installs `libuvcpp.so` with the custom OpenSSL 3.6+ lib
  directory in its RUNPATH (`CMAKE_INSTALL_RPATH_USE_LINK_PATH=ON`). Previously the dynamic
  linker resolved `libssl` from the system OpenSSL 3.0 and the daemon failed with
  `OPENSSL_3.5.0 not found` unless `LD_LIBRARY_PATH` was set manually. Verified by rebuilding the
  QUIC SDK, relinking uvscope and running raw QUIC echo plus an HTTP/3 request without env
  overrides.

## [0.8.0] - 2026-10-23

### Added

- Local TLS certificate authority and certificate renewal. An admin can create one root CA
  (`POST /api/tls/ca`, root under `<data>/ca/` with a 0600 key) and issue server certificates
  signed by it (`signedBy: "ca"` on the existing generate endpoint). Issued bundles contain the
  leaf and root, are clamped to the CA's remaining lifetime, and record `source: "ca"` plus the
  CA subject. `POST /api/tls/certificates/:id/renew` rebuilds generated and CA-signed
  certificates in place with the same id, paths and key type, reading SANs back from the current
  leaf; imported certificates are refused. Existing proxy configs stay valid; restart a proxy to
  load renewed files. `GET /api/tls/ca` reports status and
  `GET /api/tls/ca/cert` downloads the root. The TLS Inspector gains a local CA card, an issuer
  selector in the generate dialog, and a renew action per certificate. Self-check covers the CA
  lifecycle and the smoke test verifies issued and renewed chains with `openssl verify`.
- Reverse-proxy WebSocket upgrades now use the same routing rules and upstream selection as
  HTTP: method/host/header conditions and rewrite destinations are applied, and the weighted
  upstream list is filtered by active health checks. Redirect rules are skipped because the 101
  handshake has already been sent. Upgrade log entries record `action: "upgrade"`, `status: 101`,
  the chosen upstream and the matched rule. The smoke test routes an upgrade through a redirect
  rule, a header-gated rewrite rule and a dead upstream to prove the behavior.
- Large downloads are resumable. A failed transfer keeps its partial file and reports
  `resumable: true`; `POST /api/http/download/:id/resume` continues from the staged byte count
  with `Range: bytes=N-`, validates the `206 Content-Range`, and appends the remainder. If the
  upstream ignores Range with `200`, the file is truncated and fetched again; `416` and other
  errors keep the partial data. Transfers that end short of the declared `Content-Length` now
  become errors instead of false `ready` results. Snapshots add `attempts`, and the HTTP Studio
  offers Resume and Dismiss actions for a failed download. `/download/:id` serves completed
  files as 200 regardless of the last upstream status. The smoke test drops a 256 KiB response
  after 64 KiB, resumes it, and compares the final bytes with the source payload.

## [0.7.0] - 2026-10-22

### Added

- Long-lived reverse proxies are supervised: the daemon records the desired run state when a
  proxy is started or stopped, a watchdog restarts desired services whose runtime stops, and
  `GET /api/supervisor` reports desired/running/restart counts and recent events. Admins can
  restart a service or force a runtime stop through `POST /api/supervisor/:id/restart` and
  `POST /api/supervisor/:id/stop-runtime`; Settings shows the supervisor card. This is
  in-process supervision — process isolation and IPC for the runtimes remain future work.

## [0.6.1] - 2026-10-22

### Added

- HTTP Studio request scripts: a pre-request script can mutate method, URL, headers and body and
  set environment variables; a post-response script receives status, headers, text and parsed
  JSON, can set variables and record named `uvscope.test()` assertions whose PASS/FAIL results
  appear in the Scripts tab. Scripts are stored in localStorage and run in the page; the SPA CSP
  now allows `'unsafe-eval'` for this feature.

## [0.6.0] - 2026-10-22

### Added

- HTTP workspaces: named environments with `{{variable}}` substitution in the URL, headers and
  body (including urlencoded and multipart text fields), plus named collections of saved
  requests that can be reloaded. The workspace is stored in SQLite settings and exposed through
  `GET/PUT /api/http/workspace`.
- Import OpenAPI/Swagger and Postman collections from JSON into a collection (method, URL,
  header parameters, body examples where present). cURL and HAR import/export remain available.

## [0.5.2] - 2026-10-22

### Added

- HTTP Studio can export the current request as a cURL command and import one (supports `-X`,
  `-H`, `-d/--data*`, `--url`, `-k`, `-u`, `-b`, `-A`, `-e`, `--socks5-hostname` and `-F`).
  It also exports the last response as HAR 1.2 and imports the first request from a HAR file.
- Administrators can download a consistent SQLite backup (`GET /api/backup`) and upload a
  replacement (`POST /api/backup/restore`). A restore is staged next to the live database and
  applied on the next daemon start; the previous file is kept as
  `uvscope.db.pre-restore-<timestamp>`.

## [0.5.1] - 2026-10-22

### Added

- HTTP Studio request bodies have three modes: raw (the existing JSON/XML/text/binary editor),
  `application/x-www-form-urlencoded` built from key/value rows, and `multipart/form-data` with
  text fields and file inputs (files are read in the browser, max 8 MiB). Binary bodies are sent
  to the daemon as base64 (`bodyEncoding: "base64"`) and forwarded byte-for-byte.
- Experimental QUIC transport: `POST /api/quic/probe` connects with TLS 1.3/ALPN, opens one
  bidirectional stream, sends a payload and returns the reply; QUIC servers can be created,
  started, stopped and inspected through `/api/quic/servers`, echo streams/datagrams by default,
  and use a configured certificate/key pair or generate a self-signed certificate in memory.
  The client does not verify certificates unless `verify: true` is set. The Transport page has a
  QUIC mode with ALPN/verify controls and a server manager. This requires an SDK built with
  `UVCPP_ENABLE_QUIC=ON` and OpenSSL 3.6+; release builds report `quic: false` and disable it.
- `/api/health` reports build capabilities (`http2`, `http3`, `quic`, `brotli`). The HTTP Studio
  disables H2/H3 when the running SDK lacks them and explains the requirement in the tooltip;
  HTTP/3 and QUIC remain experimental and are disabled in release builds.

## [0.5.0] - 2026-10-21

### Added

- Audit log and active session management. Security-relevant actions (setup, login success/failure,
  logout, password change, session revocation, user create/update/delete, settings updates, proxy
  create/update/delete/start/stop) are recorded with the actor, target, IP and timestamp.
  `GET /api/audit` (admin) returns the log; `GET /api/auth/sessions` lists active sessions and
  `DELETE /api/auth/sessions/:id` revokes one (admins may revoke any session, users their own).
  The Users & Roles page shows both cards.
- Reverse proxies pass WebSocket upgrades through: the client handshake is answered locally and
  relayed to the first configured upstream (`ws://`, or `wss://` for an `https://` target) with
  the client's non-hop-by-hop headers, preserving path and query. Text and binary messages, and
  close codes/reasons, are relayed in both directions. Routing rules and weighted selection do
  not apply to upgrades; TLS to the upstream is verified strictly.
- Proxies with more than one upstream now check each upstream actively: a background probe
  performs a TCP/TLS connect every 10 seconds (3-second timeout), marks the result with latency
  and the last error, and selection skips unhealthy upstreams while at least one healthy
  upstream remains. `/api/proxies` exposes the per-upstream `health` array and the proxy card
  warns when an upstream is unhealthy.
- Each reverse proxy reports rolling metrics over a 60-second window: QPS, p50/p95 latency, and
  2xx/3xx/4xx/5xx status counts alongside the existing totals. The proxy card shows QPS and the
  latency percentiles; samples are kept in memory (2048 per instance) and reset when the daemon
  restarts.

## [0.4.4] - 2026-10-21

### Added

- Reverse proxies accept multiple weighted upstreams (`upstreams: [{url, weight}]`) and a retry
  count (0–3). Requests pick an upstream by weight; a failed connection fails over for any
  method, while send failures and 502/503/504 retry only idempotent methods. Access logs and
  502 responses include the upstream and attempt count, and the editor exposes the primary
  weight, additional upstreams and retry attempts.
- Reverse proxy rules can match on HTTP method, Host and headers in addition to the path. Method
  lists are comma-separated and case-insensitive; Host supports exact values and `*.suffix`
  wildcards (port ignored unless specified); `headerName` requires a header and `headerValue`
  pins its exact value. Conditions combine with AND, and an empty path pattern matches every
  path, so rules can be driven by method/host/header alone. The proxy editor exposes the new
  fields and the smoke test covers method, header, host and a combined rule.

## [0.4.3] - 2026-10-21

### Added

- WebSocket debugging can run inside the daemon: `POST /api/ws/sessions` connects to a
  `ws://`/`wss://` URL with custom handshake headers, a subprotocol and an optional SOCKS5 proxy.
  Sessions expose `open`, `text`, `binary`, `frame` (opcode, FIN/RSV1/mask, close code), `closing`,
  `close` and `error` events plus byte/frame counters; `send` supports text, binary, ping and pong,
  and `close` sends a chosen close code/reason. The WebSocket view gains a Daemon/Browser mode
  switch, a handshake-header editor and a Delete action. libuvcpp 1.8.2 has no handshake-header
  API, so a minimal patch (`patches/upstream/0005-ws-client-handshake-headers.patch`) adds
  `set_handshake_header()`; the smoke echo server rejects handshakes without the header.
- Large responses can be streamed to disk and downloaded without the 4 MiB probe clip.
  `POST /api/http/download` starts a background fetch (HTTP/1.1), `GET /api/http/download/:id`
  reports progress, and `/download/:id` serves the staged file as an attachment with
  `Content-Length` and range support. Jobs are per-user, capped at 512 MiB, cancellable with
  `DELETE`, and expire after 10 minutes. The HTTP Studio download button uses this path
  automatically when the probe body was truncated.

## [0.4.2] - 2026-10-21

### Added

- HTTP probes decode gzip, deflate and brotli responses by default (`decompress: true`, with
  `compression` kept as an alias). Results report `contentEncoding`, `decoded`, `wireBodySize` and
  the decoded `bodySize`; disabling decoding keeps the compressed bytes and the original
  `content-encoding` header. The SDK now builds brotli from source (`UVCPP_ENABLE_BROTLI=OFF`
  opts out), and the smoke test covers gzip, deflate, brotli and the raw path.

## [0.4.1] - 2026-10-21

### Fixed

- HTTP Studio previews now load sites that gate on the User-Agent (for example `baidu.com`):
  requests without an explicit `user-agent` use a current browser UA with a `uvscope/<version>`
  token, and the editor's default `accept` header matches a browser. Explicit headers still win.
- The daemon no longer selects Vite's development `web/index.html` (which loads `/src/main.ts`)
  as its document root when a built `web/dist` exists, so the UI and preview frames load when
  running from a repository checkout.
- Deleting a terminal now stops the shell, reaps the child, closes the PTY and removes the
  session from the list. It previously stayed in a closing state, reappeared after refresh and
  could leave a zombie process behind.

## [0.4.0] - 2026-10-21

### Changed

- Build against the libuvcpp 1.8.2 release SDK. The local `patches/upstream/` set (TLS SNI,
  POSIX sync I/O timeouts, complete `cmake --install`, SOCKS5 proxy stage) is retired because
  1.8.1/1.8.2 ship those fixes. Configure now fails early when the detected SDK is older than
  1.8.2, and the TLS live-chain / SOCKS5 session-stat paths are always compiled instead of
  depending on a capability probe. The managed SDK also enables HTTP/2 (nghttp2); HTTP/3/QUIC
  stay closed.

### Added

- Virtual terminal UI with xterm.js (truecolor, alternate screen, mouse, bracketed paste),
  multiple terminal tabs, session restore after refresh, automatic reconnect and admin
  all-users view. A WebSocket stream (`/api/terminal/ws/:id`) carries binary output and JSON
  input/resize frames; the PTY is SIGWINCH-nudged on attach so TUI apps repaint.
- Windows virtual terminal backend via ConPTY (`CreatePseudoConsole`, dynamically loaded for
  MinGW): PowerShell default shell, libuv pipe I/O, resize/redraw, process reaping and the same
  WebSocket/UI/session semantics as Unix. CI runs a real ConPTY echo/exit test.
- Virtual terminal quick bar and command library: a seeded SQLite catalog of ~100 Linux and ~70
  Windows commands with categories, bilingual descriptions, search, copy/run actions, per-user
  custom snippets, pin-to-quick-bar and admin restore/edit/delete.
- Virtual terminal backend (Unix): persistent per-user PTY sessions with sequenced 256 KiB
  scrollback, input/resize/output APIs, owner/admin access and a 200 ms reaper. The `terminal`
  module is disabled by default and limited to admin/operator roles; sessions survive browser
  disconnects for the daemon lifetime.
- HTTP probe results now expose the negotiated stream ID and `retryable`, plus GOAWAY state when
  the build enables HTTP/2. The timing panel shows both; HTTP/1.1 reports 0 / false.
- HTTP/2 over TLS is enabled in the default SDK: `protocol: "http2"` negotiates ALPN `h2`, and
  the probe returns a summary of the first 200 received frames (type, flags, stream, payload
  length, RST_STREAM/GOAWAY error codes). The timing panel lists the frame summary. The smoke
  test runs a real h2 request against a local Node.js server; lean builds can opt out with
  `UVCPP_ENABLE_NGHTTP2=OFF`.
- TLS Inspector decodes every certificate in a PEM bundle (up to 32 displayed) and lists the
  chain in input order with per-certificate validity and fingerprints.
- TLS Inspector can fetch a live `https://` URL and show the peer chain, ALPN, verification
  result/error and handshake error. Invalid certificates are still displayed with the OpenSSL
  reason. Requires the libuvcpp 1.8.2 SDK (the build minimum).
- TLS certificate vault: generate self-signed RSA/ECDSA server certificates with SANs, import
  certificate/key pairs (encrypted keys supported), list/inspect/download/delete, 0600 key files
  and in-use protection. The reverse proxy editor can select a managed certificate.
- SOCKS5 server sessions show per-session bytes, UDP datagram counts and duration
  (`bytesAvailable: true`).
- HTML preview has an opt-in “Run scripts (unsafe)” mode backed by short-lived `/preview/:id`
  documents with `CSP: sandbox allow-scripts`. It renders JS-driven pages and remote CSS/JS on an
  opaque origin without uvscope cookies; the default sandbox still blocks scripts.
- Reverse proxy routing rules: ordered exact / prefix / regex matches with HTTP redirects
  (301/302/307/308) or upstream path rewrites. Destinations support `$uri`, `$args`,
  `$request_uri`, `$rest`, `$1`–`$9`, `$host` and `$scheme`, plus optional query preservation.
  Rules are edited in the proxy modal and persisted in `proxies.rules_json`.
- Reverse proxy start failures now show the real reason in the UI: bind errors map to
  "address already in use", TLS/certificate failures populate `lastError`, and create/edit reports
  the start warning instead of a success toast.
- SOAP / WSDL starts with the webxml QQ online-check service as the default test WSDL.
- Safe preview now rewrites CSS `@import` URLs. Enabling remote assets switches to a `/preview`
  document with its own CSP so external CSS, fonts and frames load (a `srcdoc` document inherits
  the SPA CSP, which would block them).

## [0.3.0] - 2026-10-20

### Added

- Persistent TCP sessions: create a connection, send repeatedly (text or hex), inspect a
  per-session event log, and close it explicitly. Sessions run on a dedicated libuv loop so a
  stalled target cannot block the control plane.
- `GET/POST /api/tcp/sessions`, `GET /api/tcp/sessions/:id/events`, `POST .../send` and
  `DELETE /api/tcp/sessions/:id`.
- Transport panel with session state, byte counters, queued writes and an incremental event log.
- Smoke coverage for a two-send TCP echo round-trip on one persistent connection.
- TCP and UDP server listeners with start/stop/delete, persisted `autoStart`, accepted TCP
  sessions, UDP datagram timelines, per-session/per-datagram send, and a dedicated
  `TCP / UDP Servers` page.
- `/api/transport/listeners` routes for listener lifecycle, events, accepted sessions and sends.
- SOCKS5 servers (CONNECT / BIND / UDP ASSOCIATE) with no-auth or RFC 1929 username/password
  authentication, persisted `autoStart`, start/stop/delete, event and session logs, and a dedicated
  `SOCKS5 Server` page. Passwords are stored as PBKDF2 hashes.
- `/api/socks5/servers` routes for lifecycle, events and connection attempts.
- Smoke coverage for SOCKS5 CONNECT and BIND round-trips, `curl --socks5-hostname`, wrong-password
  rejection, an unsupported command (`REP=7`) and an authenticated round-trip.
- SOCKS5 outbound proxy for HTTP, TCP and UDP probes and persistent TCP sessions, with optional
  RFC 1929 credentials. Proxy failures are classified (`proxy_unreachable`, `auth_failed`,
  `target_rejected`, `timeout`, `protocol_error`) and never fall back to a direct connection.
- Upstream patch `0004-socks5-proxy-stage` exposes whether a SOCKS5 failure happened while
  connecting to the proxy or after it replied (libuvcpp PR
  [#42](https://github.com/Antruly/libuvcpp/pull/42)).
- Smoke coverage for TCP, HTTP and UDP through a local SOCKS5 server plus error classification and
  no-direct-fallback assertions.
- Application icon derived from the web favicon: embedded in `uvscope.exe`, used by the Windows
  installer and shortcuts, and installed into the Linux hicolor theme with `Icon=uvscope` in the
  desktop entry. macOS ships `uvscope.icns` in the install tree.

### Changed

- Version bumped to 0.3.0 across CMake, npm, the Windows installer, documentation and the UI
  fallback.

## [0.2.0] - 2026-10-20

### Added

- HTTP response preview: sandboxed HTML, images, audio, video and PDF, with raw/hex fallback.
  HTML scripts are disabled and remote media is opt-in.
- Chinese/English UI across every workbench, with browser-language detection, a manual top-bar
  switch and a persisted choice. `<html lang>` follows the selection.
- Theme control now cycles dark, light and follow-system; `auto` reacts to `prefers-color-scheme`.
- Per-view translation fragments under `web/src/i18n/` and a lightweight `t()` store with no new
  runtime dependency.

### Changed

- The application CSP allows `blob:` for image/media/frame previews and `http:`/`https:` for the
  opt-in remote media mode. Scripts stay disabled by both the sandbox and `script-src 'none'`.
- Version bumped to 0.2.0 across CMake, npm, the Windows installer and the UI fallback.

### Fixed

- libuvcpp: TLS SNI is set on both the async and blocking client paths and omitted for IP
  literals, as RFC 6066 requires.
- libuvcpp: `timeout_ms` is honoured in the blocking `connect_wait` / `send_wait` paths on Linux
  and macOS. A silent peer no longer blocks until it closes the socket, and a timeout is no longer
  reported as `200 OK` with an empty body.
- libuvcpp: `cmake --install` now produces a usable SDK. Shared FetchContent dependencies
  (`libllhttp`) are installed, installed libraries carry `$ORIGIN` / `@loader_path` RPATH, and
  bundled libuv headers are exposed to `find_package(uvcpp)` consumers.

The libuvcpp fixes are submitted upstream as
[#38](https://github.com/Antruly/libuvcpp/pull/38),
[#39](https://github.com/Antruly/libuvcpp/pull/39) and
[#40](https://github.com/Antruly/libuvcpp/pull/40). Reproduction and verification are in
[`docs/LIBUVCPP_UPSTREAM.zh-CN.md`](docs/LIBUVCPP_UPSTREAM.zh-CN.md).

### Known limitations

- Persistent TCP/UDP listeners, repeated-send TCP sessions and SOCKS5 client/server are planned
  for 0.3; this release keeps the one-shot transport probes.
- HTTP/2 and HTTP/3 remain experimental and are disabled in release builds until the libuvcpp
  async TLS/h2/h3 matrix is green on Windows and macOS.
- Preview is a viewer, not a security boundary against resource exhaustion; bodies remain capped
  at 4 MiB.

## [0.1.0] - 2026-10-20

### Added

- First usable release of the uvscope protocol workbench.
- C++17 daemon built on libuvcpp 1.8.0 with an embedded HTTP API and Vue 3 SPA.
- First-run administrator setup, SQLite storage, PBKDF2 password hashing, server-side sessions
  and admin/operator/viewer roles.
- Vue shell with collapsible navigation, cached multi-tab workspaces, overview dashboard and live
  polling.
- HTTP Studio: arbitrary methods, headers, body, TLS verification toggle, timing, response headers,
  raw body, history and restore.
- WebSocket console: browser-native connection, text and binary/hex frames, live trace.
- TCP and UDP one-shot probes with text/hex payloads and wire transcript.
- TLS Inspector: PEM/DER X.509 decoding with subject, issuer, SAN, key usage, EKU, fingerprint and
  validity.
- SOAP/WSDL workbench: fetch or paste WSDL, operation discovery, SOAPAction, envelope generation
  and Fault display.
- Managed HTTP reverse proxy: create, edit, start/stop, delete, TLS listener paths and live logs.
- Module toggles, user management, password reset, factory reset, scheduled/manual update check,
  about and help.
- TLS Inspector accepts both PEM text and binary DER uploads.
- Linux/Windows/macOS build scaffolding, GitHub Actions CI/release workflows, CPack deb/rpm and
  Windows Inno Setup installer script.

### Fixed

- Added TLS SNI support to libuvcpp HTTP/1.1 clients via
  [`patches/upstream/0001-tls-sni.patch`](patches/upstream/0001-tls-sni.patch). Without it, HTTPS
  requests to modern virtual hosts fail or receive an unrelated certificate.

### Known limitations

- HTTP/2 and HTTP/3 are exposed as experimental protocol preferences. libuvcpp's async TLS/h2/h3
  paths still require upstream validation; HTTP/1.1 is the supported transport.
- Reverse proxies are HTTP/1.1 only and do not forward WebSocket upgrades in 0.1.0.
- TCP/UDP probes are one-shot; persistent listeners and raw QUIC streams are planned.
- Windows arm64 builds are not part of the first release matrix.
