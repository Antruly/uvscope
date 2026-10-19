# Architecture

uvscope is deliberately a **single daemon + single-page app**. The daemon owns authentication,
persistence, protocol clients and the gateway runtimes. The SPA is a view over the daemon's API.

```text
┌─────────────────────────────────────────────────────────────────────┐
│ Browser                                                             │
│  Vue 3 + Pinia + Router                                             │
│  left rail · cached tabs · telemetry polling · native WebSocket    │
└───────────────────────────────┬─────────────────────────────────────┘
                                │ same-origin HTTP / cookie session
┌───────────────────────────────▼─────────────────────────────────────┐
│ uvscope daemon (C++17)                                              │
│                                                                     │
│  uvcpp_web_app                                                      │
│   ├─ middleware: security headers, deny-by-default API 404          │
│   ├─ /api/auth, /api/setup, /api/users                              │
│   ├─ /api/http, /api/tcp, /api/udp, /api/tls                         │
│   ├─ /api/proxies, /api/history, /api/settings                      │
│   └─ static SPA (web/ dist)                                         │
│                                                                     │
│  Auth(Store)              Store (SQLite, single writer)             │
│  ProxyManager             libuv thread pool for short probes        │
│   └─ one uvcpp_web_app per proxy (dedicated event loop)             │
│                                                                     │
│  libuvcpp 1.8.0 + patch (TLS SNI)                                   │
└───────────────────────────────┬─────────────────────────────────────┘
                                │
                     HTTP(S) / TCP / UDP / TLS targets
```

## Process model in 0.1.0

- **Main daemon**: the only process. It owns the UI server, SQLite, auth and all protocol clients.
- **Short-lived work**: HTTP/TCP/UDP/TLS probes run on the libuv thread pool through `uvcpp_work`; the
  response is written back on the event-loop thread.
- **Reverse proxy runtimes**: each running proxy is a `uvcpp_web_app` with its own event loop and
  thread. Requests use a synchronous libuvcpp HTTP client on that proxy loop. This keeps proxy
  blocking isolated from the control plane, at the cost of per-instance serialization for slow
  upstreams.
- **Static assets**: served by the same `uvcpp_web_app`; SPA fallback is enabled so deep links work.

The roadmap moves persistent proxy/transport services into explicit child processes with a
supervisor and IPC so the main daemon remains responsive under many long-lived sessions.

## Request lifecycle: HTTP Studio

1. The SPA posts JSON to `/api/http/send` with URL, method, headers, body, timeout and protocol.
2. `Auth` resolves the session cookie and checks the `http` module + `debug` permission.
3. The handler copies the request into an immutable job and schedules it on the libuv thread pool.
4. The job:
   - parses the URL;
   - creates an `uvcpp_http_client`;
   - loads the CA bundle and sets `PEER_STRICT` unless insecure was requested;
   - connects and sends HTTP/1.1 through libuvcpp;
   - captures status, headers, body, negotiated protocol and timing.
5. Back on the loop thread the handler increments metrics, writes a summary to SQLite, and returns
   the JSON payload.
6. The SPA renders body/headers/timing and adds the summary to the history rail.

## Request lifecycle: reverse proxy

1. The SPA creates a proxy config through `/api/proxies`.
2. `Store` persists it. `ProxyManager` starts a `uvcpp_web_app` on the requested `host:port`.
3. Incoming requests hit the catch-all handler, which:
   - strips hop-by-hop headers;
   - appends `X-Forwarded-For`, `X-Forwarded-Host`, `X-Forwarded-Proto`, `X-uvscope-Proxy`;
   - performs a synchronous HTTP/1.1 request to the upstream;
   - copies status, headers and body back;
   - records a log entry.
4. The SPA polls `/api/proxies/:id/logs?after=<seq>` while the log panel is open.

## SQLite schema

```sql
users(id, username UNIQUE, password_hash, role, enabled, created_at, last_login)
sessions(token_hash PK, user_id, created_at, expires_at)
settings(key PK, value, updated_at)
history(id, user_id, kind, summary, status, http_status, latency_ms, size_bytes, detail, created_at)
proxies(id PK, name, listen_host, listen_port, target, auto_start, tls, cert_path, key_path,
        created_at, updated_at)
```

- WAL mode, `busy_timeout=5000`.
- The main daemon is the only writer. This is both a concurrency decision and a future
  multi-process invariant.
- History is pruned to the latest 5,000 rows on insert.
- Deleting the database (or the factory reset action) reopens a fresh file and returns to first-run
  setup.

## Authentication and authorization

- First run: only `/api/setup/status` and `/api/setup/init` are useful. Initialization creates the
  first administrator.
- Passwords: PBKDF2-HMAC-SHA256, 210,000 iterations, 16-byte random salt, 32-byte output.
- Session token: 32 random bytes, hex encoded in an HttpOnly cookie; only its SHA-256 is stored.
- Roles:
  - `admin`: everything, including users/settings/reset.
  - `operator`: debug modules, history, reverse proxy management.
  - `viewer`: dashboard, history read, about.
- Module toggles are checked server-side (`require_module`) and mirrored in navigation.

## Error model

API errors are JSON:

```json
{ "ok": false, "error": "human readable", "code": "machine_code" }
```

Probe results are also JSON and keep `ok` plus detailed timing/error fields. A protocol failure is
not an HTTP 500 for the API; it is a successful probe request whose `ok` is false.

## Dependencies

- **libuvcpp 1.8.0** — HTTP server/client, WebSocket, SQLite wrapper, WSDL/SOAP, SSL.
- **OpenSSL** — password hashing, certificate parsing and TLS (libuvcpp uses it for transport).
- **SQLite** — local state.
- **Vue 3 / Vite / Pinia / Vue Router** — SPA.
- **Inno Setup** — Windows installer.

## Known architectural gaps (0.1.0)

- HTTP/2 and HTTP/3 are not trustworthy yet; the async TLS path in libuvcpp needs upstream work.
- Proxy upstreams use a blocking client inside the proxy loop; throughput is limited.
- No WebSocket upgrade pass-through in the proxy.
- No per-path/Host routing rules; one upstream per listener.
- The `history` table stores summaries, not full bodies.
- No IPC supervisor yet; all long-lived services are threads in the daemon.
