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
| POST | `/api/http/send` | `{url, method, protocol, headers[{name,value}], body, timeoutMs, insecure, compression}` → probe result |
| POST | `/api/tcp/probe` | `{host, port, payload, encoding, connectTimeoutMs, readTimeoutMs}` |
| POST | `/api/udp/probe` | `{host, port, payload, encoding, timeoutMs}` |
| POST | `/api/tcp/sessions` | `{host, port, connectTimeoutMs}` → persistent TCP session |
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
| POST | `/api/tls/inspect` | `{pem}` → certificate profile |
| POST | `/api/proxies` | `{id?, name, listenHost, listenPort, target, autoStart, tls, certPath, keyPath, start}` |
| POST | `/api/proxies/:id/start` | Start a proxy |
| POST | `/api/proxies/:id/stop` | Stop a proxy |
| DELETE | `/api/proxies/:id` | Stop and delete a proxy |

`encoding` is `text` or `hex`. Probe results always include `ok`; protocol failures are reported in
the result rather than as an API error.

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

`POST /api/transport/listeners` binds a local port; `transport` is `tcp` or `udp`. TCP listeners
produce accepted-session records with `peer`, `state`, byte counters and a per-session event log.
UDP listeners produce `datagram` and `sent` events with `peer` and `payload`; replies use the
numeric peer host/port from the datagram log.

Listener states are `stopped`, `starting`, `listening`, `stopping` and `error`. A listener error
keeps the config; fix the port and start it again. Listener configs are stored in SQLite settings
and auto-started when `autoStart` is true. Limits mirror the TCP session limits; accepted sessions
are retained up to 128 per listener.

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
