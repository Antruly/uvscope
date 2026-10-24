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

## 0.4 — protocol depth (0.4.0)

0.4.0 ships HTTP/2 and the live certificate chain; the remaining items continue in 0.4.x.

- [x] HTTP/2 over libuvcpp 1.8.2 (ALPN, stream IDs, GOAWAY, frame summary; Linux smoke)
- [ ] HTTP/2 runtime validation on the Windows and macOS CI jobs
- [ ] HTTP/3 / QUIC client with QPACK / 0-RTT / migration events (experimental; needs OpenSSL 3.2+)
- [x] HTTP/3 results expose ALPN, stream ID and peer GOAWAY state (experimental builds)
- [x] Linux CI builds OpenSSL 3.6 + QUIC SDK and runs a local HTTP/3 origin
- [~] Raw QUIC client probe and echo server (streams/datagrams; experimental build)
- [x] Compression decoding: gzip / deflate / brotli with a raw-body toggle
- [x] Large-response streaming download (512 MiB cap, progress, cancel)
- [x] Multipart upload and resumable Range downloads
- [x] Live peer certificate chain inspection
- [x] Backend WebSocket sessions (daemon-side connect, headers, ping/pong, close codes, frames)
- [ ] WebSocket backend relay / pass-through mode

## 0.5 — gateway and team workflow

- [x] Exact / prefix / regex routing with redirects and path rewrites
- [x] Header / method / host routing conditions
- [x] Multiple weighted upstreams and retry policy
- [x] WebSocket pass-through
- [x] Active upstream health checks
- [x] Certificate import / self-sign / selection
- [x] Per-instance QPS, latency and traffic metrics
- [x] Collections, environments and variable substitution
- [x] Pre-request / post-response scripts (browser-side, experimental)
- [x] cURL, HAR, OpenAPI and Postman import/export (HAR export is one entry)
- [x] Audit log and active session management
- [x] Backup download and staged restore (migrations still ad hoc)
- [~] In-process service supervisor with desired-state restarts
- [x] Proxy upstream I/O off the proxy loop (bounded per-instance worker pool)
- [x] Configurable upstream connect/request timeouts and worker count
- [ ] Process isolation + IPC for long-lived services

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
- [x] Local CA hierarchy and certificate renewal
