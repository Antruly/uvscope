# Roadmap

uvscope 1.0.0 is the first stable release: the functional surface is complete, and the release
matrix, documentation and supply-chain artifacts are in place. Work after 1.0 is additive.

The detailed history and per-slice acceptance notes are in
[`docs/DEVELOPMENT_PLAN.zh-CN.md`](docs/DEVELOPMENT_PLAN.zh-CN.md); user-facing changes are in
[`CHANGELOG.md`](CHANGELOG.md).

## 1.0 — stable (released 2026-10-25)

### Product surface

- [x] HTTP/1.1 and HTTPS debugging with history, body modes, cURL/HAR import/export
- [x] HTTP/2 over TLS with ALPN, stream IDs, GOAWAY and frame summaries
- [x] WebSocket console: daemon-side backend sessions and browser-native mode
- [x] TCP/UDP probes, persistent TCP sessions and TCP/UDP listeners
- [x] TLS decoding/probing, certificate vault, local CA and CA-signed certificates
- [x] SOAP 1.1 + WSDL workbench
- [x] Managed reverse proxy: routing rules, weighted upstreams, retries, health checks,
      circuit breaker, metrics, hot reload and WebSocket pass-through
- [x] SOCKS5 client and server (CONNECT / BIND / UDP ASSOCIATE)
- [x] Virtual terminal (Unix PTY / Windows ConPTY) with an offline command library
- [x] Workspaces, collections, environments, pre/post scripts and assertions
- [x] Users/RBAC, server-side sessions, audit log, backup/restore and module toggles
- [x] Chinese/English UI and light/dark/system themes

### Isolation and supervision

- [x] Child-process isolation for reverse proxies, TCP/UDP listeners, SOCKS5 servers and terminals
- [x] Length-prefixed JSON IPC, per-service telemetry and pid/snapshot proxies
- [x] One desired-state supervisor with backoff, restart counters and an event log
- [x] Stop/delete/module-disable/reset paths unregister before teardown
- [x] Terminal crash recovery with a fresh shell and WebSocket `reset` continuity

### Release engineering

- [x] Matrix: Linux x64/arm64, Windows x64 (MinGW), macOS arm64
- [x] Linux: ctest + full smoke; Windows: self-check + ConPTY + HTTP/2 runtime smoke;
      macOS: self-check + HTTP/2 runtime smoke
- [x] Performance baselines: `scripts/bench.sh` and `docs/PERFORMANCE.md`
- [x] Stable event schema and extension contract: `docs/EVENTS.md`, `docs/EXTENDING.md`
- [x] Documentation site (MkDocs Material on GitHub Pages, built in CI)
- [x] Supply chain: `SHA256SUMS`, Sigstore keyless signatures and an SPDX SBOM on every release

## After 1.0

### Distribution and platform

- [ ] Windows arm64 build and smoke (MSYS2 CLANGARM64 is still preview)
- [ ] Signed and notarized macOS `.app` bundle (the 1.x macOS asset is a Homebrew-linked tar.gz)
- [ ] More Linux package targets beyond tar.gz/deb/rpm
- [ ] Guided TLS setup for the control plane (currently `--tls-cert` / `--tls-key`)

### Protocol depth

- [ ] HTTP/3/QUIC: QPACK summaries, 0-RTT and connection-migration events; stabilize once the
      OpenSSL 3.6+ requirement fits the default toolchain
- [ ] Dedicated WebSocket relay / pass-through listener mode
- [ ] Fully asynchronous proxy upstream client (replace the bounded blocking worker pool)
- [ ] Raw QUIC stream/datagram workbench parity with TCP/UDP

### Operations and scale

- [ ] Versioned SQLite migrations with explicit downgrade/rollback guidance
- [ ] Concurrent saturation benchmarks (latency at 1/10/100 connections) tracked in CI
- [ ] Optional per-child resource limits (CPU, memory, file descriptors)
- [ ] Restart budget / circuit breaker for permanently failing supervised services

### Extensibility and UX

- [ ] Native plugin ABI (the 1.x boundary is the HTTP API, scripts and the internal IPC contract)
- [ ] Page-by-page light-theme color audit for every workbench
- [ ] Broader documentation translations based on community demand
- [ ] Collections/environments sharing beyond a single workspace file

## Shipped before 1.0

The 0.1–0.11 series delivered the workbench, transport, gateway, certificate and isolation
foundations. See the [changelog](CHANGELOG.md) for the release-by-release record and
[`docs/DEVELOPMENT_PLAN.zh-CN.md`](docs/DEVELOPMENT_PLAN.zh-CN.md) for the slice-level design and
verification notes (including S13–S19 for IPC, isolation and supervision).
