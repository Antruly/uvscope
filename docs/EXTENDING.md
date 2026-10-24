# Extending uvscope

uvscope does not load native plugins in 1.x. Extensibility is provided by four stable, documented
surfaces: environment variables, request scripts, the command library and the HTTP API. The
child-process IPC protocol is an internal boundary between uvscope processes, not a public ABI.

## Environment variables

Environments live in the HTTP workspace (`GET/PUT /api/http/workspace`) and are selected per
workspace. Variables use double braces and are substituted in the request URL, header names and
values, and the request body:

```text
{{base_url}}/v1/users
Authorization: Bearer {{token}}
```

The substitution pattern is `{{ name }}` with `[\w.-]+` names. Unknown variables are left literal
so a typo is visible in the outgoing request. The active environment can be changed without
re-saving a request; the raw request text is preserved.

## Pre-request and post-response scripts

Scripts run in the browser as async functions with `"use strict"`. They are trusted local code:
unlike the response preview, they are not sandboxed and can call browser APIs. The workspace
request and response are passed through the `uvscope` argument.

### Pre-request

```js
const url = new URL(uvscope.request.url)
url.searchParams.set('trace', '1')
uvscope.request.url = url.toString()
uvscope.request.headers.push({ name: 'X-Debug', value: '1' })
uvscope.env.set('last_trace', '1')
```

| Surface | Description |
| --- | --- |
| `uvscope.request.method` | Mutable method |
| `uvscope.request.url` | Mutable URL (variables already substituted) |
| `uvscope.request.headers` | Mutable `[{ name, value }]` array |
| `uvscope.request.body` | Mutable body text |
| `uvscope.env.get(name)` / `uvscope.env.set(name, value)` | Read or write the active environment |
| `uvscope.variables` | Read-only snapshot of the active environment |

Returning normally sends the (possibly modified) request. A thrown error aborts the send and is
shown in the UI. The script may be async; `await` is supported.

### Post-response

```js
uvscope.test('status is 200', () => {
  if (uvscope.response.status !== 200) throw new Error('unexpected status')
})
uvscope.test('json has items', () => {
  if (!Array.isArray(uvscope.response.json().items)) throw new Error('items missing')
})
if (uvscope.response.status === 401) uvscope.env.set('token', '')
```

| Surface | Description |
| --- | --- |
| `uvscope.response.status` | Numeric status |
| `uvscope.response.headers` | `[{ name, value }]` |
| `uvscope.response.text` | Body text |
| `uvscope.response.contentType` | `Content-Type` header or `""` |
| `uvscope.response.json()` | Parsed JSON (throws on invalid JSON) |
| `uvscope.test(name, fn)` | Runs `fn`; a throw records a failed assertion |
| `uvscope.env.get/set`, `uvscope.variables` | Same as pre-request |

Post-response scripts run only for successful requests (`res.ok`). Script source is stored in
browser `localStorage` per workspace; it is not written to SQLite and is not sent to the daemon.

## Command library

The terminal command catalog is seeded into SQLite on first migration. Custom entries are created
with `POST /api/terminal/commands`:

```json
{
  "name": "Tail app log",
  "command": "tail -f /var/log/app.log",
  "category": "custom",
  "platform": "linux",
  "description": "Follow the application log",
  "quick": true
}
```

`platform` is `linux`, `windows` or empty (both), `category` groups the list, `description` is
shown in the command detail, and `quick: true` pins the entry to the quick bar. Admins can edit
or delete any entry and restore deleted built-ins with
`POST /api/terminal/commands/restore`. Commands are plain text; uvscope never executes a command
without an explicit user action in a terminal session.

## Proxy destination variables

Rule destinations (`redirect` / `rewrite`) support the variables documented in the
[HTTP API](API.md#reverse-proxy-routing-rules): `$uri`, `$args`, `$request_uri`, `$rest`,
`$1`–`$9`, `$host` and `$scheme`. Unknown variables are left literal. This is the supported way to
express routing logic without a plugin.

## HTTP API

Every UI feature is backed by the same JSON API documented in [API.md](API.md). Responses use
`{ ok, ... }` with a typed `code` on errors, timestamps are ISO-8601 UTC, and event streams are
sequenced for incremental polling. Event and payload compatibility rules are in
[EVENTS.md](EVENTS.md). Integrations should use the API rather than driving the UI.

## Child-process IPC (internal)

Isolated services are launched with hidden flags (`--proxy-child`, `--listener-child`,
`--socks5-child`, `--terminal-child`, `--ipc-child`). The parent and child exchange frames over a
Unix domain socket (Windows named pipe):

```text
4-byte big-endian length | UTF-8 JSON payload (maximum 4 MiB)
```

Each service defines its own message types (`start`, `input`, `resize`, `redraw`, `close`, `stop`,
`snapshot`, `output`, `events`, `send`, `session-events`, …) and an initial `ready`/`started`
handshake. The protocol is versioned with the daemon: a child is always the same executable and
build as its parent. It is documented here for operators and contributors, but it is **not** a
stable third-party plugin ABI and may change between minor releases.

## What is not in 1.x

- A native plugin ABI or dynamically loaded modules.
- User-authored server-side code execution.
- A remote API authentication mode other than the workbench session cookie.

These are tracked as post-1.0 work in the [roadmap](https://github.com/Antruly/uvscope/blob/main/ROADMAP.md).
