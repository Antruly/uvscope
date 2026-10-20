# Changelog

All notable changes to uvscope are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this project adheres to
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed

- HTTP Studio previews now load sites that gate on the User-Agent (for example `baidu.com`):
  requests without an explicit `user-agent` use a current browser UA with a `uvscope/<version>`
  token, and the editor's default `accept` header matches a browser. Explicit headers still win.
- The daemon no longer selects Vite's development `web/index.html` (which loads `/src/main.ts`)
  as its document root when a built `web/dist` exists, so the UI and preview frames load when
  running from a repository checkout.

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
