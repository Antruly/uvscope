# HTTP API

All endpoints are same-origin JSON. Authenticated endpoints use the `uvscope_session` HttpOnly
cookie. Errors are `{ "ok": false, "error": "...", "code": "..." }`.

## Public

| Method | Path | Description |
| --- | --- | --- |
| GET | `/api/health` | Liveness, version, libuvcpp version and setup flag |
| GET | `/api/setup/status` | First-run status and data directory |
| POST | `/api/setup/init` | Create the first administrator (only before initialization) |
| POST | `/api/auth/login` | Username/password login; sets the session cookie |

## Authenticated

| Method | Path | Permission | Description |
| --- | --- | --- | --- |
| GET | `/api/auth/me` | dashboard | Current user, module flags and permissions |
| POST | `/api/auth/logout` | cookie | Delete the session |
| POST | `/api/auth/password` | dashboard | Change the current password and rotate the session |
| GET | `/api/dashboard` | dashboard | Server/metrics/activity snapshot |
| GET | `/api/history?kind=&limit=` | history.read | Recent activity summaries |
| DELETE | `/api/history?kind=` | admin.reset | Clear one or all history kinds |
| GET | `/api/proxies` | dashboard | Proxy configuration + runtime state |
| GET | `/api/proxies/:id/logs?after=&limit=` | dashboard | Incremental proxy event log |

## Debug modules

Each module must be enabled in Settings and the role must have `debug`.

| Method | Path | Body / result |
| --- | --- | --- |
| POST | `/api/http/send` | `{url, method, protocol, headers[{name,value}], body, timeoutMs, insecure, decompress, proxy}` → probe result |
| POST | `/api/http/download` | Same body as `/api/http/send` → background download job |
| GET | `/api/http/download/:id` | Download progress/state |
| DELETE | `/api/http/download/:id` | Cancel and delete a download (owner/admin) |
| GET | `/download/:id` | Staged file (public by token, attachment only) |
| POST | `/api/ws/sessions` | `{url, headers[], subprotocol, insecure, timeoutMs, proxy}` → backend WebSocket session |
| GET | `/api/ws/sessions?all=` | List sessions (admin `all=1` sees every user) |
| GET | `/api/ws/sessions/:id` | Session snapshot and counters |
| GET | `/api/ws/sessions/:id/events?after=&limit=` | Incremental frame/message/close events |
| POST | `/api/ws/sessions/:id/send` | `{type: text\|binary\|ping\|pong, payload, encoding}` |
| POST | `/api/ws/sessions/:id/close` | `{code, reason}` → graceful Close frame |
| DELETE | `/api/ws/sessions/:id` | Close and remove the session |
| POST | `/api/tcp/probe` | `{host, port, payload, encoding, connectTimeoutMs, readTimeoutMs, proxy}` |
| POST | `/api/udp/probe` | `{host, port, payload, encoding, timeoutMs, proxy}` |
| POST | `/api/tcp/sessions` | `{host, port, connectTimeoutMs, proxy}` → persistent TCP session |
| GET | `/api/tcp/sessions` | List retained TCP sessions |
| GET | `/api/tcp/sessions/:id` | One session snapshot |
| GET | `/api/tcp/sessions/:id/events?after=&limit=` | Incremental session event log |
| POST | `/api/tcp/sessions/:id/send` | `{payload, encoding}` → queue a write |
| DELETE | `/api/tcp/sessions/:id` | Close a session |
| GET | `/api/transport/listeners` | List configured TCP/UDP listeners and runtime state |
| POST | `/api/transport/listeners` | `{transport, name, host, port, autoStart, start}` |
| GET | `/api/transport/listeners/:id` | One listener snapshot |
| POST | `/api/transport/listeners/:id/start` | Start a listener |
| POST | `/api/transport/listeners/:id/stop` | Stop a listener |
| DELETE | `/api/transport/listeners/:id` | Stop and delete a listener |
| GET | `/api/transport/listeners/:id/events?after=&limit=` | Listener event log |
| GET | `/api/transport/listeners/:id/sessions` | Accepted TCP sessions |
| GET | `/api/transport/listeners/:id/sessions/:sid/events?after=&limit=` | Accepted session event log |
| POST | `/api/transport/listeners/:id/send` | TCP: `{sessionId, payload, encoding}`; UDP: `{peerHost, peerPort, payload, encoding}` |
| DELETE | `/api/transport/listeners/:id/sessions/:sid` | Close one accepted TCP session |
| GET | `/api/socks5/servers` | List configured SOCKS5 servers and runtime state |
| POST | `/api/socks5/servers` | `{name, host, port, username, password, autoStart, start}` |
| GET | `/api/socks5/servers/:id` | One SOCKS5 server snapshot |
| POST | `/api/socks5/servers/:id/start` | Start a SOCKS5 server |
| POST | `/api/socks5/servers/:id/stop` | Stop a SOCKS5 server |
| DELETE | `/api/socks5/servers/:id` | Stop and delete a SOCKS5 server |
| GET | `/api/socks5/servers/:id/events?after=&limit=` | Server event log |
| GET | `/api/socks5/servers/:id/sessions` | CONNECT / BIND / UDP ASSOCIATE attempts |
| POST | `/api/tls/inspect` | `{pem, encoding}` → certificate profile; PEM bundles return `certificates[]` |
| POST | `/api/tls/probe` | `{url, timeoutMs, insecure}` → live peer chain + verification |
| GET | `/api/tls/certificates` | Managed certificate metadata (no private keys) |
| POST | `/api/tls/certificates/generate` | `{name, commonName, sans[], days, keyType, keyBits}` (admin) |
| POST | `/api/tls/certificates/import` | `{name, certPem, keyPem, password}` (admin) |
| GET | `/api/tls/certificates/:id` | Metadata plus parsed certificate chain |
| GET | `/api/tls/certificates/:id/cert` | Download the certificate PEM |
| DELETE | `/api/tls/certificates/:id` | Delete a managed certificate (admin) |
| POST | `/api/preview` | `{html, baseUrl, mode: safe\|scripts}` → `{id, url}` preview token (8 MiB, 120 s) |
| DELETE | `/api/preview/:id` | Drop a preview token |
| GET | `/preview/:id` | Token-based HTML with `CSP: sandbox allow-scripts` (no auth) |
| POST | `/api/proxies` | `{id?, name, listenHost, listenPort, target, autoStart, tls, certPath, keyPath, rules[], start}` |
| POST | `/api/proxies/:id/start` | Start a proxy |
| POST | `/api/proxies/:id/stop` | Stop a proxy |
| DELETE | `/api/proxies/:id` | Stop and delete a proxy |

`encoding` is `text` or `hex`. Probe results always include `ok`; protocol failures are reported in
the result rather than as an API error.

`/api/http/send` results include `streamId` and `retryable` (0 / false on HTTP/1.1). A request
without an explicit `user-agent` is sent with a current browser UA plus a `uvscope/<version>`
token, so sites that serve bot or JS-redirect shims to unknown clients still return their normal
HTML; an explicit header always wins.

`decompress` (default `true`) advertises `accept-encoding: gzip, deflate, br` unless the request
already has one, and decodes the response body. The result then carries:

```json
"compression": {
  "requested": true,
  "contentEncoding": "br",
  "decoded": true,
  "wireBodySize": 27,
  "bodySize": 2800
}
```

`wireBodySize` is the compressed size, `bodySize` the decoded size. When decoding is disabled with
`decompress: false`, the body stays compressed (`bodyEncoding: "base64"`), `content-encoding` is
preserved in `headers`, and no `compression` object is returned. An unsupported encoding (for
example `zstd`) leaves the body untouched and adds `compression.error`. The older `compression`
field is accepted as an alias for `decompress`.

With
`protocol: "http2"` over `https://`, the daemon negotiates ALPN `h2` and also returns
`goaway: {received, errorCode, lastStreamId}` plus a frame summary:

```json
"h2": {
  "frameCount": 4,
  "framesTruncated": false,
  "maxFrames": 200,
  "frames": [
    {"type": "SETTINGS", "typeId": 4, "flags": 0, "streamId": 0, "length": 0,
     "errorCode": 0, "lastStreamId": 0},
    {"type": "HEADERS", "typeId": 1, "flags": 4, "streamId": 1, "length": 34,
     "errorCode": 0, "lastStreamId": 0}
  ]
}
```

`frames` holds the first 200 **received** frames (payload bytes are not captured), in arrival
order; `frameCount` is the total seen and `framesTruncated` marks the cap. `type` uses the RFC
9113 names, `flags` is the raw bit mask, and `errorCode` / `lastStreamId` are meaningful only on
`RST_STREAM` / `GOAWAY`. An SDK built without nghttp2 makes the daemon reject `protocol: "http2"`
with `this libuvcpp build has no HTTP/2 support`.

### Large response downloads

`POST /api/http/download` accepts the same body as `/api/http/send` (the `protocol`,
`decompress` and `maxBodyBytes` fields are ignored; the transfer is HTTP/1.1 and the bytes are
stored as received). It returns immediately:

```json
{"ok": true, "download": {
  "id": "9f2c…", "state": "running", "bytes": 0, "totalBytes": -1,
  "filename": "big.bin", "url": "/download/9f2c…"
}}
```

Poll `GET /api/http/download/:id` for `state` (`running`, `ready`, `error`, `cancelled`) and the
byte counters. When `ready`, `GET /download/:id` serves the file with
`Content-Disposition: attachment`, `Content-Length` and byte-range support. The URL is public by
unguessable token, but the file is always sent as `application/octet-stream` so target HTML can
never execute on the uvscope origin. Jobs belong to their creator (admins see all), are capped
at 512 MiB, can be cancelled with `DELETE /api/http/download/:id`, and are pruned 10 minutes
after they finish. Staged files do not survive a daemon restart.

### Backend WebSocket sessions

`POST /api/ws/sessions` connects from the daemon to a `ws://` or `wss://` URL. `headers` are added
to the Upgrade request (`Authorization`, `Cookie`, `Origin`, `Sec-WebSocket-Protocol`, …); the
standard handshake headers are reserved. `subprotocol` is a shortcut for
`Sec-WebSocket-Protocol`, `insecure` disables TLS verification and `proxy` takes the same SOCKS5
object as the HTTP probe. Each session runs on its own thread and loop.

Snapshots carry `state` (`connecting`, `open`, `closing`, `closed`, `error`), byte/frame counters,
close code/reason and `nextSeq`. Event types:

- `connecting`, `open`, `closing`, `close` (`code`, `reason`), `error`;
- `sent` (`opcode`, `size`);
- `text` (`text`, `encoding`, `size`, `truncated`) and `binary` (`base64`, `size`, `truncated`);
- `frame` (`opcode`, `opcodeId`, `fin`, `rsv1`, `masked`, `length`, optional `text`/`base64`,
  `truncated`, and `closeCode`/`closeReason` on CLOSE). Frames are received frames only; payloads
  are capped at 16 KiB in the event, while messages are capped at 256 KiB.

`send` accepts `type: "text" | "binary" | "ping" | "pong"` and `encoding: "text" | "hex" |
"base64"` (hex/base64 are decoded before sending). `close` sends a Close frame with the requested
code and reason; a locally initiated close settles as `closed` once the peer answers or after a
five-second grace period. `DELETE` sends `1001` and hides the session immediately.

`POST /api/preview` stores a captured HTML document in memory (max 8 entries, 8 MiB each,
120 s TTL) and returns a random token. `mode: "safe"` allows remote CSS/images/fonts/frames but
keeps `script-src 'none'`; `mode: "scripts"` additionally runs scripts. `GET /preview/:id` is
public by token and always responds with `Content-Security-Policy: sandbox ...` in addition to the
iframe sandbox, so the document runs on an opaque origin with no access to uvscope cookies,
storage or API responses. The default `srcdoc` preview remains script-free; remote assets switch to
the safe `/preview` document so they are not blocked by the SPA's own CSP.

## Persistent TCP sessions

A session is created with `POST /api/tcp/sessions` and stays open until `DELETE` or the peer closes
it. It runs on a dedicated libuv loop, so a stalled target never blocks the control plane.

```json
{
  "ok": true,
  "session": {
    "id": "…",
    "transport": "tcp",
    "host": "127.0.0.1",
    "port": 9000,
    "state": "connected",
    "error": "",
    "closeReason": "",
    "createdAt": "2026-10-20T…Z",
    "connectedAt": "2026-10-20T…Z",
    "closedAt": "",
    "sentBytes": 10,
    "receivedBytes": 20,
    "nextSeq": 6,
    "queuedWrites": 0,
    "queuedBytes": 0
  }
}
```

`state` is `connecting`, `connected`, `closing`, `closed` or `error`. Sends are queued; the
`send` event is appended only after libuv reports the write complete. Event types are `created`,
`connected`, `send`, `recv`, `peer-closed`, `closed` and `error`; every event has `seq` and `at`.
Data events carry `payload` with `length`, `encoding` (`utf8` or `base64`), `text` or `base64`,
`hex`, and `truncated` (payloads over 64 KiB are clipped for the event log).

Limits: 64 live sessions, 1 MiB per payload, 1 MiB queued per session, 512 KiB / 2,000 events
retained per session. Closed sessions remain visible until the retained list exceeds 128.

## TCP / UDP listeners

### SOCKS5 outbound proxy

`/api/http/send`, `/api/tcp/probe`, `/api/udp/probe` and `/api/tcp/sessions` accept an optional
`proxy` object:

```json
{"proxy": {"host": "127.0.0.1", "port": 1080, "username": "", "password": ""}}
```

- HTTP and TCP use SOCKS5 CONNECT; UDP uses UDP ASSOCIATE. The target hostname is sent to the
  proxy and never resolved locally.
- A configured proxy is mandatory: a failed handshake fails the request and never falls back to a
  direct connection.
- `username` empty means no-auth. A password without a username is rejected with
  `invalid_config` rather than silently downgrading to no-auth.
- Failures include `proxy.errorClass`: `invalid_config`, `proxy_unreachable`, `auth_failed`,
  `target_rejected`, `timeout`, `protocol_error` or `proxy_error`, plus `proxy.errorCode`
  (libuv) and `proxy.error` (the proxy's own message).
- HTTP/3 and SOCKS5 cannot be combined; that combination is rejected explicitly.

### Live TLS certificate probe

`POST /api/tls/probe` accepts `{url, timeoutMs, insecure}` and connects to an `https://` URL without
sending an HTTP request. It returns the captured certificate profile (`certificates[]`, leaf fields
at the top level) plus `live`:

```json
{"verified": true, "verifyResult": 0, "verifyError": "ok", "alpn": "", "insecure": false,
 "handshakeError": "", "certificateCount": 4}
```

- The chain is returned even when verification fails (self-signed, expired, wrong host), so the
  UI can inspect the certificate; `verified` is false and `verifyError` carries the OpenSSL text.
- `insecure: true` disables verification for the handshake itself.
- The build requires libuvcpp 1.8.2, which provides `peer_cert_chain_der`.
- HTTP/3 is not supported by this probe; ALPN is reported when the build enables HTTP/2.

`POST /api/socks5/servers` binds a local SOCKS5 port. Leaving `username` and `password` empty
enables no-auth; providing both enables RFC 1929 username/password authentication. The password is
stored as a PBKDF2 hash and is never returned. SOCKS5 servers default to `127.0.0.1` and must be
moved to a LAN address explicitly. Sessions report the requested command, target, peer, status and
error. The libuvcpp 1.8.2 session-end callback adds `bytesClientToTarget`,
`bytesTargetToClient`, `udpDatagrams`, `udpBytes`, `durationMs`, `endedAt` and `endStatus`.

`POST /api/transport/listeners` binds a local port; `transport` is `tcp` or `udp`. TCP listeners
produce accepted-session records with `peer`, `state`, byte counters and a per-session event log.
UDP listeners produce `datagram` and `sent` events with `peer` and `payload`; replies use the
numeric peer host/port from the datagram log.

Listener states are `stopped`, `starting`, `listening`, `stopping` and `error`. A listener error
keeps the config; fix the port and start it again. Listener configs are stored in SQLite settings
and auto-started when `autoStart` is true. Limits mirror the TCP session limits; accepted sessions
are retained up to 128 per listener.

### Managed certificates

`POST /api/tls/certificates/generate` creates a self-signed server certificate (RSA 2048/3072/4096
or ECDSA P-256/P-384) with SAN entries, `serverAuth` EKU and `CA:FALSE`. `POST .../import`
accepts a PEM certificate (leaf first, intermediates optional) and a matching private key;
encrypted keys are decrypted with the optional `password`. Files live under `<data>/certs/<id>/`:
`cert.pem`, `key.pem` (0600) and `meta.json`. The API never returns key material, only local
`certPath`/`keyPath` values that the reverse proxy editor can select.

Deleting a certificate still referenced by a proxy returns `409 certificate_in_use`.

## Reverse proxy routing rules

`POST /api/proxies` accepts an ordered `rules` array (max 64). The first enabled matching rule
wins. A rule is:

```json
{
  "id": "optional; generated when empty",
  "name": "old docs",
  "enabled": true,
  "matchType": "exact | prefix | regex",
  "pattern": "/old",
  "action": "redirect | rewrite",
  "status": 301,
  "destination": "/new",
  "preserveQuery": true
}
```

- Matching uses the raw request path (without the query). `exact` compares the whole path,
  `prefix` is a byte-prefix match, `regex` uses ECMAScript `std::regex_search`.
- `redirect` answers with `status` (301, 302, 303, 307 or 308) and a `Location` header without
  contacting the upstream. `rewrite` replaces the upstream path and continues proxying; the configured target
  path is not prepended for a rewritten request.
- Destination variables: `$uri` (path), `$args` (query), `$request_uri` (path + query),
  `$rest` (remainder after a prefix match), `$1`–`$9` (regex captures), `$host` (request Host),
  `$scheme` (`http`/`https`). Unknown variables are left literal.
- With `preserveQuery: true`, the original query is appended unless the destination already
  contains `?`; destinations using `$args` or `$request_uri` are not appended twice.
- Invalid regexes, rewrite destinations that do not start with `/` or `$`, empty patterns or more
  than 64 rules are rejected with `400 bad_request`.

## Virtual terminals

The `terminal` module (Unix PTY or Windows ConPTY) is disabled by default; enable it in Settings. Routes require the `terminal`
permission (admin and operator) and an enabled module. Sessions are owned by the creating user and
survive browser disconnects for the lifetime of the daemon.

| Method | Path | Description |
| --- | --- | --- |
| GET | `/api/terminal/sessions` | Own sessions; admins may pass `?all=1` |
| POST | `/api/terminal/sessions` | `{name, shell, cwd, cols, rows}` → session |
| GET | `/api/terminal/sessions/:id` | One session (owner or admin) |
| DELETE | `/api/terminal/sessions/:id` | Close a session (owner or admin) |
| POST | `/api/terminal/sessions/:id/input` | `{data, encoding}` where encoding is text, hex or base64 |
| POST | `/api/terminal/sessions/:id/resize` | `{cols, rows}` |
| GET | `/api/terminal/sessions/:id/output?after=&limit=` | Ordered output chunks, base64 encoded |
| WS | `/api/terminal/ws/:id` | Live stream: binary output frames, JSON control/input frames |
| GET | `/api/terminal/commands?platform=&category=&q=&all=` | Command catalog (seeded + own custom) |
| POST | `/api/terminal/commands` | Add a custom command `{name, command, category, platform, description, quick}` |
| PATCH | `/api/terminal/commands/:id` | Edit a command or toggle `quick` |
| DELETE | `/api/terminal/commands/:id` | Delete a command |
| POST | `/api/terminal/commands/restore` | Re-insert deleted built-in commands (admin) |

Session states are `starting`, `running`, `closing`, `exited` and `error`. Output chunks carry
`seq` and base64 `data`; `nextSeq`, `firstSeq`, `truncated` and `missed` describe the bounded
scrollback (256 KiB). `shell` must be an absolute executable; `cwd` must be an absolute directory.
Limits: 4 running sessions per user, 32 total.

The daemon seeds a command catalog into SQLite on first migration (Linux and Windows, grouped by
system/CPU/disk/network/process/users/files/packages/services/logs/security/containers/development).
Built-ins and custom snippets are searchable by command, name, description and tags. Custom
commands belong to their creator; admins can edit/delete any entry and restore deleted built-ins.
`quick: true` pins an entry to the terminal quick bar.

The WebSocket upgrade checks the session cookie, module flag, role, terminal ownership and the
`Origin` header. Server messages:

- binary frames: terminal output bytes (feed them to a UTF-8 terminal emulator);
- `{"type":"attached","id":...}` on attach, followed by the retained scrollback;
- `{"type":"reset"}` when the client fell behind the scrollback ring;
- `{"type":"exit","state":"exited","code":0}` after the process exits;
- `{"type":"error","error":"..."}` for input failures.

Client messages: `{"type":"input","data":"...","encoding":"text|hex|base64"}`,
`{"type":"resize","cols":100,"rows":30}` and `{"type":"ping"}`. The server sends SIGWINCH on
attach so full-screen programs repaint.

## Administration

| Method | Path | Description |
| --- | --- | --- |
| GET | `/api/settings` | Modules, update policy, data directory |
| PUT | `/api/settings` | `{modules, autoUpdateCheck, updateIntervalHours}` |
| GET | `/api/users` | List users |
| POST | `/api/users` | `{username, password, role}` |
| PATCH | `/api/users/:id` | `{role?, enabled?, password?}` |
| DELETE | `/api/users/:id` | Delete a user |
| POST | `/api/reset` | `{module}` where module is `http`, `tcpudp`, `tls`, `proxy`, `history` or `all` |

`module: "all"` stops proxies, removes the SQLite database, recreates the schema and returns
`{ "ok": true, "setupRequired": true }`.

## About / updates

| Method | Path | Description |
| --- | --- | --- |
| GET | `/api/about` | Version, libuvcpp/libuv, repository, cached stars and capabilities |
| GET | `/api/update/check?force=1` | Latest GitHub release from `Antruly/uvscope` |

The release check is cached for 15 minutes and can also run automatically at the configured
interval. It uses only the public GitHub API and sends no project or user data.
