# Changelog

All notable changes to uvscope are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this project adheres to
[Semantic Versioning](https://semver.org/).

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
- Module toggles, user management, password reset, factory reset, update check, about and help.
- Linux/Windows/macOS build scaffolding, GitHub Actions CI/release workflows, CPack deb/rpm and
  Windows Inno Setup installer script.

### Fixed

- Added TLS SNI support to libuvcpp HTTP/1.1 clients via
  [`patches/libuvcpp-1.8.0-sni.patch`](patches/libuvcpp-1.8.0-sni.patch). Without it, HTTPS
  requests to modern virtual hosts fail or receive an unrelated certificate.

### Known limitations

- HTTP/2 and HTTP/3 are exposed as experimental protocol preferences. libuvcpp's async TLS/h2/h3
  paths still require upstream validation; HTTP/1.1 is the supported transport.
- Reverse proxies are HTTP/1.1 only and do not forward WebSocket upgrades in 0.1.0.
- TCP/UDP probes are one-shot; persistent listeners and raw QUIC streams are planned.
- Windows arm64 builds are not part of the first release matrix.
