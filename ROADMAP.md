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

## 0.1.x — hardening

- [ ] Mark HTTP/2 / HTTP/3 as experimental in the UI and hide them when not compiled
- [ ] Smoke test in CI: setup → login → HTTP probe → proxy round-trip
- [ ] Proxy upstream timeout and WebSocket upgrade detection
- [ ] Better binary/body viewer and downloadable responses
- [ ] Windows and macOS runtime DLL/dylib bundling verification
- [ ] Submit the TLS SNI fix upstream to libuvcpp

## 0.2 — protocol depth

- [ ] Reliable HTTP/2 over libuvcpp (ALPN, stream IDs, GOAWAY, frame summary)
- [ ] HTTP/3 / QUIC client with QPACK / 0-RTT / migration events
- [ ] Raw QUIC stream and datagram debugging
- [ ] Compression decoding and streaming large bodies
- [ ] Multipart upload and Range download
- [ ] Live peer certificate chain inspection
- [ ] WebSocket ping/pong, close-code and backend relay mode

## 0.3 — gateway

- [ ] Host/path/header/method routing rules
- [ ] WebSocket pass-through
- [ ] Header and path rewrites
- [ ] Multiple upstreams and retry policy
- [ ] Certificate import / self-sign / selection
- [ ] Per-instance QPS, latency and traffic metrics
- [ ] Persistent TCP/UDP listeners with session timelines
- [ ] Supervisor + IPC for long-lived services

## 0.4 — team workflow

- [ ] Collections, environments and variable substitution
- [ ] Pre-request / post-response scripts
- [ ] HAR, cURL, OpenAPI and Postman import/export
- [ ] Audit log and active session management
- [ ] Backup / restore / migrations
- [ ] Scheduled health checks
- [ ] Command palette and keyboard navigation

## 1.0 — stable

- [ ] Linux x64/arm64, Windows x64/arm64 (MinGW), macOS arm64
- [ ] Signed releases and SBOM
- [ ] Full test matrix and performance baselines
- [ ] Stable event schema and plugin boundary
- [ ] Documentation site
