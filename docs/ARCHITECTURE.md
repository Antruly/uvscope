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
│   ├─ /api/tcp/sessions (persistent transport sessions)               │
│   ├─ /api/transport/listeners (TCP/UDP server listeners)              │
│   ├─ /api/socks5/servers (SOCKS5 CONNECT/BIND/UDP ASSOCIATE)          │
│   ├─ /api/proxies, /api/history, /api/settings                      │
│   └─ static SPA (web/ dist)                                         │
│                                                                     │
│  Auth(Store)              Store (SQLite, single writer)             │
│  ProxyManager             libuv thread pool for short probes        │
│   └─ one uvcpp_web_app per proxy (dedicated event loop)             │
│  TransportManager                                                   │
│   └─ one libuv loop thread; TCP sessions + command queue            │
│  Socks5Manager                                                      │
│   └─ one libuv loop thread per running SOCKS5 server                │
│                                                                     │
│  libuvcpp 1.8.0 + patch (TLS SNI)                                   │
└───────────────────────────────┬─────────────────────────────────────┘
                                │
                     HTTP(S) / TCP / UDP / TLS targets
```

## Process model in 0.3.0

- **Main daemon**: the only process. It owns the UI server, SQLite, auth and all protocol clients.
- **Short-lived work**: HTTP/TCP/UDP/TLS probes run on the libuv thread pool through `uvcpp_work`; the
  response is written back on the event-loop thread.
- **Reverse proxy runtimes**: each running proxy is a `uvcpp_web_app` with its own event loop and
  thread. Requests use a synchronous libuvcpp HTTP client on that proxy loop. This keeps proxy
  blocking isolated from the control plane, at the cost of per-instance serialization for slow
  upstreams.
- **Persistent transport sessions**: `TransportManager` owns one dedicated libuv loop on its own
  thread. Web handlers enqueue commands and read mutex-protected snapshots; sessions never run on
  the control-plane loop. A 500 ms sweep enforces connect timeouts and retires closed clients.
- **TCP/UDP listeners**: the same transport loop hosts `uvcpp_tcp_server` listeners and raw
  `uv_udp_t` sockets. TCP accepted clients stay server-owned; the manager only observes them and
  queues writes. Listener configs live in SQLite; runtime state and bounded event logs live in the
  manager.
- **SOCKS5 servers**: each running server is a `uvcpp_socks5_server` with its own loop and thread.
  Commands are marshaled through `uvcpp_async`; snapshots and bounded logs are mutex-protected.
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
7. Renderable responses can switch to Preview: HTML is injected into a sandboxed `srcdoc`
   iframe, media/PDF use Blob URLs, and unknown or truncated bodies stay raw/hex.

## Request lifecycle: reverse proxy

1. The SPA creates a proxy config through `/api/proxies`.
2. `Store` persists it. `ProxyManager` starts a `uvcpp_web_app` on the requested `host:port`.
3. Incoming requests hit the catch-all handler, which:
   - evaluates the ordered routing rules (exact/prefix/regex; redirect or path rewrite) and
     returns the redirect or rewrites the upstream path on the first match;
   - strips hop-by-hop headers;
   - appends `X-Forwarded-For`, `X-Forwarded-Host`, `X-Forwarded-Proto`, `X-uvscope-Proxy`;
   - performs a synchronous HTTP/1.1 request to the upstream;
   - copies status, headers and body back;
   - records a log entry (including the matched rule for redirects).
4. The SPA polls `/api/proxies/:id/logs?after=<seq>` while the log panel is open.

## SQLite schema

```sql
users(id, username UNIQUE, password_hash, role, enabled, created_at, last_login)
sessions(token_hash PK, user_id, created_at, expires_at)
settings(key PK, value, updated_at)
history(id, user_id, kind, summary, status, http_status, latency_ms, size_bytes, detail, created_at)
proxies(id PK, name, listen_host, listen_port, target, auto_start, tls, cert_path, key_path,
        rules_json, created_at, updated_at)
```

- WAL mode, `busy_timeout=5000`.
- The main daemon is the only writer. This is both a concurrency decision and a future
  multi-process invariant.
- History is pruned to the latest 5,000 rows on insert.
- Deleting the database (or the factory reset action) reopens a fresh file and returns to first-run
  setup.

## Persistent TCP sessions

- `POST /api/tcp/sessions` allocates an id and returns `connecting` immediately; the connect runs
  on the transport loop.
- Each session has a single async write in flight. `send` requests are queued (1 MiB per session)
  and a `send` event is appended only when libuv reports the write complete.
- `recv`, `peer-closed`, `closed` and `error` events are appended to a bounded per-session log
  (2,000 events / 512 KiB, payloads clipped at 64 KiB).
- A 500 ms sweep enforces the connect timeout and deletes retired client wrappers after their close
  callbacks have run.
- Session state is plain data guarded by a mutex; libuv objects stay on the transport thread.

## Listener runtime

- TCP listeners use `uvcpp_tcp_server` on the transport loop. Accepted clients remain owned by the
  server; the manager holds non-owning pointers, queues one write at a time, and relies on the
  server for close/delete.
- UDP listeners use `uv_udp_t` with `uv_udp_recv_start`; replies use `uv_udp_send` to the numeric
  peer address.
- Listener configs are persisted under the `transport.listeners` settings key and auto-started
  when requested.
- Stopping a listener closes the listener handle and all accepted clients; the server object is
  retired and deleted by the 500 ms sweep after its close callbacks have run.

## SOCKS5 outbound proxy

- `Socks5Proxy` is carried by HTTP/TCP/UDP probes and persistent sessions. HTTP/TCP use
  `set_socks5_proxy()` (CONNECT); UDP uses `uvcpp_socks5_client` with UDP ASSOCIATE.
- Target hostnames are passed to the proxy as `ATYP=DOMAIN`; no local resolution is performed.
- There is no direct fallback: any configured proxy failure ends the request.
- A small upstream patch (`patches/upstream/0004-socks5-proxy-stage.patch`) exposes whether the
  last SOCKS5 failure happened while connecting to the proxy or after the proxy replied. Without
  it, "connection refused" from the proxy and from the target are indistinguishable; the patch
  makes `proxy_unreachable` and `target_rejected` reliable.

## SOCKS5 server runtime

- Configs persist under the `socks5.servers` settings key. Passwords are stored as PBKDF2 hashes;
  the plaintext never reaches SQLite.
- `uvcpp_socks5_server` owns its event loop, so each running server gets one dedicated thread.
  Start/stop commands cross threads through `uvcpp_async` and are executed on the loop thread.
- Server handshakes, CONNECT/BIND upstreams and UDP relays all stay inside libuvcpp. The control
  plane only reads snapshots and the bounded event/session logs.
- A watchdog `start` wait fails after five seconds; port conflicts land in `state: error` with the
  OS message, and deleting the config stops the runtime first.
- libuvcpp reports a session once, when the requested command is established or rejected. It does
  not expose per-session byte counters or close notifications, so the UI states that limitation
  instead of guessing counters.

## Response preview safety

- The default preview iframe uses `sandbox=""`: scripts, forms, popups and top-level navigation
  are off.
- An injected CSP adds `script-src 'none'`, `connect-src 'none'` and `frame-src 'none'`.
- Remote images/audio/video are blocked until the user ticks “Load remote assets”. When it is on,
  the preview switches from `srcdoc` to `/preview/:id` with `mode: "safe"`: a real HTTP document
  with its own CSP, so remote CSS, fonts and frames are not blocked by the SPA's stricter policy
  (a `srcdoc` document inherits that policy). Scripts remain off in this mode.
- Relative URLs are resolved against the response URL: `srcdoc` rewrites `src`/`href`/`srcset`/
  CSS `url()` and `@import`; `/preview` injects `<base href="response URL">` under a CSP that
  permits that base.
- “Run scripts (unsafe)” posts the captured HTML to `/api/preview` with `mode: "scripts"` and
  embeds `/preview/:id`. That response carries `CSP: sandbox allow-scripts` and is therefore an
  opaque origin even when opened directly: scripts can run and load remote resources, but they
  cannot read uvscope cookies, storage or API responses. Tokens are random, in-memory, capped and
  expire after 120 s.
- The scripted document receives a `<base href="response URL">` so relative CSS/JS/fetch URLs
  resolve like the original page. Target login state is not transferred; authenticated pages may
  still render logged-out.
- Blob URLs are revoked when the response or tab changes; preview tokens are deleted on switch.
- Preview is a viewer, not a security boundary against resource exhaustion; the body is still
  capped at 4 MiB.

## Internationalization and theme

- `web/src/i18n.ts` holds the base dictionary; per-view fragments live in `web/src/i18n/*.ts`.
- `useI18nStore` detects `navigator.language`, persists the manual choice in `localStorage`, and
  keeps `<html lang>` in sync.
- Theme is `dark`, `light` or `auto`; `auto` follows `prefers-color-scheme` through a media-query
  listener. Both choices are client-only convenience state.

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

## Known architectural gaps (0.3.0)

- HTTP/2 and HTTP/3 remain opt-in and experimental; the libuvcpp async TLS/h2 path works in local
  Linux testing but still needs the full Windows/macOS matrix.
- Persistent TCP sessions and TCP/UDP listeners are implemented; SOCKS5 client/server is next.
- Preview cannot execute scripts and does not stream bodies larger than the probe cap.
- Proxy upstreams use a blocking client inside the proxy loop; throughput is limited.
- No WebSocket upgrade pass-through in the proxy.
- No per-path/Host routing rules; one upstream per listener.
- The `history` table stores summaries, not full bodies.
- No IPC supervisor yet; all long-lived services are threads in the daemon.
