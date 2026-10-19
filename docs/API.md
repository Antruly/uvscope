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
| POST | `/api/tls/inspect` | `{pem}` → certificate profile |
| POST | `/api/proxies` | `{id?, name, listenHost, listenPort, target, autoStart, tls, certPath, keyPath, start}` |
| POST | `/api/proxies/:id/start` | Start a proxy |
| POST | `/api/proxies/:id/stop` | Stop a proxy |
| DELETE | `/api/proxies/:id` | Stop and delete a proxy |

`encoding` is `text` or `hex`. Probe results always include `ok`; protocol failures are reported in
the result rather than as an API error.

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
