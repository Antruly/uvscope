# uvscope

**A local-first, multi-protocol web debugging workbench and gateway.**

uvscope puts HTTP, WebSocket, TCP, UDP, TLS, SOAP/WSDL and managed reverse proxies behind one
Vue 3 control plane. A C++ daemon owns the event loop, SQLite store, authentication and outbound
traffic; long-lived services can move to supervised child processes, and the browser only renders
state and sends commands.

![uvscope dashboard](screenshots/dashboard.png)

## Start here

<div class="grid cards" markdown>

- **[Build and install](BUILDING.md)** — requirements, the libuvcpp SDK, packaging and the
  Windows installer.
- **[HTTP API](API.md)** — every route, payload and state model, including terminals and the
  service supervisor.
- **[Architecture](ARCHITECTURE.md)** — process model, request flows, SQLite schema and the
  long-lived runtimes.
- **[Operations](OPERATIONS.md)** — data directory, backup/restore, logs, isolation and hardening.

</div>

## What it does

- **HTTP Studio** — HTTP/1.1 and HTTP/2 (ALPN, stream IDs, GOAWAY, frame summaries), body modes,
  cURL/HAR import and export, environments, collections, scripts and assertions, compression,
  streaming downloads and a sandboxed response preview.
- **Realtime consoles** — WebSocket with daemon-side backend sessions or browser-native mode,
  one-shot TCP/UDP probes and persistent TCP sessions.
- **Gateway** — reverse proxies with exact/prefix/regex routing, rewrites, weighted upstreams,
  retries, active health checks, per-upstream circuit breakers, metrics, hot reload and WebSocket
  pass-through.
- **Long-lived services** — TCP/UDP listeners, SOCKS5 servers, reverse proxies and terminal
  PTYs can run in isolated child processes under one desired-state supervisor.
- **Security tooling** — X.509 decode/probe, managed certificate vault, local CA, CA-signed
  certificates and in-place renewal.
- **Operations** — SQLite persistence, first-run admin, RBAC, server-side sessions, audit log,
  backup/restore, module toggles and one-command reset.

## Isolation in one minute

Set `isolated: true` when creating a reverse proxy, listener, SOCKS5 server, or terminal session.
The service runs in a `--*-child` process; the parent keeps the API state and talks to it over a
length-prefixed JSON pipe.

All four kinds register with the same supervisor:

- a crashed child is restarted with exponential backoff and a new PID;
- `GET /api/supervisor` reports `desired`, `running`, `restarts` and `lastError`;
- stopping a service clears its desired state; deleting it unregisters it first;
- a terminal shell `exit` is not a crash — the child stays alive for scrollback and exit status;
  a killed child is respawned with a fresh shell and WebSocket clients get a `reset` frame.

## Release

v1.0.1 is the current stable release. Assets, `SHA256SUMS`, the Sigstore signature and the SPDX
SBOM are on the [Releases page](https://github.com/Antruly/uvscope/releases/tag/v1.0.1).

| Platform | Assets |
| --- | --- |
| Linux x64 / arm64 | `.tar.gz`, `.deb`, `.rpm` |
| Windows x64 (MinGW) | `.zip`, `uvscope-1.0.1-setup.exe` |
| macOS arm64 | `.tar.gz` (Homebrew `libuv` and `openssl@3`) |

HTTP/3 and raw QUIC are available in QUIC-enabled builds; the default release assets keep QUIC
off. Windows arm64 is
planned for a post-1.0 release.

## Project links

- [Repository](https://github.com/Antruly/uvscope)
- [Roadmap](https://github.com/Antruly/uvscope/blob/main/ROADMAP.md)
- [Changelog](https://github.com/Antruly/uvscope/blob/main/CHANGELOG.md)
- [Contributing](https://github.com/Antruly/uvscope/blob/main/CONTRIBUTING.md)
- [Security policy](https://github.com/Antruly/uvscope/blob/main/SECURITY.md)
