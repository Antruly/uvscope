# Roadmap

uvscope is developed in small, verifiable slices. The detailed plan is in
[`docs/DEVELOPMENT_PLAN.zh-CN.md`](docs/DEVELOPMENT_PLAN.zh-CN.md).

## 0.1.0 — first usable workbench

- [x] Local daemon, SQLite, first-run admin, sessions, RBAC
- [x] Vue 3 shell with collapsible menu and cached tabs
- [x] HTTP/1.1 + HTTPS request debugger with history
- [x] WebSocket console
- [x] TCP/UDP one-shot probes
- [x] TLS certificate decoder
- [x] SOAP 1.1 / WSDL workbench
- [x] Managed HTTP reverse proxies with live logs
- [x] Module switches, reset, update check, about/help
- [x] CI, Linux packages, Windows installer scaffold
- [x] libuvcpp SNI, POSIX sync timeouts and install-completeness patches
      ([PR #38](https://github.com/Antruly/libuvcpp/pull/38),
      [#39](https://github.com/Antruly/libuvcpp/pull/39),
      [#40](https://github.com/Antruly/libuvcpp/pull/40))

## 0.2 — debug experience

- [x] Response preview for HTML (sandboxed), images, audio, video and PDF
- [x] Preview safety: no scripts, no remote resources by default, clear limits
- [x] Chinese/English shell, navigation, login and setup; browser-language detection and manual switch
- [x] Light / dark / system theme switching
- [ ] Page-by-page light-theme color audit for every workbench
- [x] Raw / preview / hex switching for binary and truncated bodies

## 0.3 — transport workbench

- [x] Persistent TCP client sessions (send repeatedly, stream receives, close, reconnect)
- [x] TCP server listeners with session list, send and close
- [x] UDP server listeners with datagram timeline and replies
- [x] Listener persistence and optional auto-start
- [x] SOCKS5 client for HTTP/TCP/UDP with authentication and error classification
- [x] SOCKS5 server (CONNECT / BIND / UDP ASSOCIATE) with authentication and session logs
- [x] No silent direct fallback when a proxy is configured

## 0.4 — protocol depth

- [x] HTTP/2 over libuvcpp 1.8.2 (ALPN, stream IDs, GOAWAY, frame summary; Linux smoke)
- [ ] HTTP/2 runtime validation on the Windows and macOS CI jobs
- [ ] HTTP/3 / QUIC client with QPACK / 0-RTT / migration events
- [ ] Raw QUIC stream and datagram debugging
- [ ] Compression decoding and streaming large bodies
- [ ] Multipart upload and Range download
- [ ] Live peer certificate chain inspection
- [ ] WebSocket ping/pong, close-code and backend relay mode

## 0.5 — gateway and team workflow

- [ ] Host/path/header/method routing rules
- [ ] WebSocket pass-through
- [ ] Header and path rewrites
- [ ] Multiple upstreams and retry policy
- [ ] Certificate import / self-sign / selection
- [ ] Per-instance QPS, latency and traffic metrics
- [ ] Collections, environments and variable substitution
- [ ] Pre-request / post-response scripts
- [ ] HAR, cURL, OpenAPI and Postman import/export
- [ ] Audit log and active session management
- [ ] Backup / restore / migrations
- [ ] Supervisor + IPC for long-lived services

## 1.0 — stable

- [ ] Linux x64/arm64, Windows x64/arm64 (MinGW), macOS arm64
- [ ] Signed releases and SBOM
- [ ] Full test matrix and performance baselines
- [ ] Stable event schema and plugin boundary
- [ ] Documentation site

## Post-0.4 additions

- [x] Build against the libuvcpp 1.8.2 release SDK and retire the local upstream patches
- [x] Reverse proxy exact/prefix/regex redirect and rewrite rules
- [x] Live TLS peer-chain probe and managed certificate vault (generate/import/delete)
- [x] Unix virtual terminal with WebSocket, xterm.js, persistent sessions and admin control
- [x] Seeded SQLite command library with quick bar, search, custom snippets and quick pins
- [x] Windows ConPTY terminal backend
- [ ] Local CA hierarchy and certificate renewal
