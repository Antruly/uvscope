# Event schema

uvscope exposes several bounded, sequenced event streams. The HTTP API reads them with
`?after=<seq>&limit=<n>`; long-lived WebSocket terminals stream output separately. This page is the
1.x compatibility contract for those payloads.

## Compatibility policy

- Within 1.x, event fields may be **added**; existing field names, types and meanings do not
  change without a major release.
- Consumers must ignore unknown fields and unknown `type` values.
- `seq` is monotonic per stream and starts at 1. It is the cursor for `after`. Gaps mean the
  bounded buffer dropped older entries, not that the event was lost silently: the stream's
  `firstSeq`/`nextSeq` (where exposed) describe the retained window.
- Timestamps are ISO-8601 UTC strings. Proxy logs use `time`; other streams use `at`.
- Payloads are truncated at a fixed byte budget. Truncation is always explicit in the payload
  (`truncated: true`) and never changes the event type.
- No stream includes credentials, private keys or complete request/response bodies by default.

## Common envelope

Every stream event carries:

```json
{
  "seq": 12,
  "at": "2026-10-25T09:30:00Z",
  "type": "connected"
}
```

Supervisor events add `id` (service id), `name` and an optional `message`. Proxy logs add
`method`, `path`, `status` and optional `rule`, `action`, `latencyMs`, `error`, `message`.

## Service supervisor

`GET /api/supervisor/events?after=&limit=` returns:

| `type` | Meaning | Extra fields |
| --- | --- | --- |
| `registered` | A service was added to the watchdog | `message` describes desired state |
| `unregistered` | A service was removed (delete, module disable, reset) | — |
| `enabled` / `disabled` | `desired` changed | — |
| `restart-requested` | `POST /api/supervisor/:id/restart` | — |
| `fail-runtime` | `POST /api/supervisor/:id/stop-runtime` | — |
| `restart` | The watchdog started a desired service that was down | `message` |

Snapshot rows (`GET /api/supervisor`) contain `id`, `kind` (`proxy`, `listener`, `socks5`,
`terminal`), `name`, `desired`, `running`, `restarts`, `lastError` and `updatedAt`.

## Persistent TCP sessions

`GET /api/tcp/sessions/:id/events?after=&limit=`:

| `type` | Extra fields |
| --- | --- |
| `connected` | — |
| `recv` | `payload` (see below) |
| `send` | `payload` |
| `closed` | `reason` |
| `peer-closed` | `fin` |
| `error` | `error` |

### Payload objects

Binary-safe payloads use one shape in TCP/UDP/listener events:

```json
{
  "length": 12,
  "truncated": false,
  "hex": "68 65 6c 6c 6f 0a ...",
  "encoding": "utf8",
  "text": "hello"
}
```

`encoding` is `utf8` or `base64` (binary payloads use `base64` instead of `text`). `length` is the
original byte count; `truncated` means the retained sample is shorter than `length`.

## TCP / UDP listeners

`GET /api/transport/listeners/:id/events?after=&limit=`:

| `type` | Extra fields |
| --- | --- |
| `started` | — |
| `stopped` | — |
| `error` | `error` |
| `accepted` | `sessionId`, `peer` |
| `session-closed` | `sessionId` (see the per-session `closed`/`error` event for the reason) |
| `datagram` | `peer`, `partial`, `payload` |

Per-session events (`.../sessions/:sid/events`) use the same envelope:

| `type` | Extra fields |
| --- | --- |
| `accepted` | `peer` |
| `recv` | `payload` |
| `send` | `payload` |
| `closed` | `reason` |
| `peer-closed` | `fin` |
| `error` | `error` |

`peer` is `"host:port"`. UDP datagrams also report `partial` when the datagram was clipped at the
receive buffer size.

## SOCKS5 servers

`GET /api/socks5/servers/:id/events?after=&limit=`:

| `type` | Extra fields |
| --- | --- |
| `started` | `port` (actual bound port) |
| `stopped` | — |
| `error` | `error` |
| `session-end` | `sessionId`, `command`, `target`, `peer`, `endStatus`, `durationMs`, `bytesClientToTarget`, `bytesTargetToClient`, `udpDatagrams`, `udpBytes` |

Session records (`.../sessions`) use the same fields plus `state`, `endedAt`,
`bytesAvailable` and `error` when the session failed.

## Reverse proxy logs

`GET /api/proxies/:id/logs?after=&limit=` returns entries with `seq`, `time`, `method`, `path`,
`status` and, when relevant, `rule`, `action`, `latencyMs`, `error` or `message`. `method` is
`SYSTEM` for lifecycle entries (started, stopped, hot reload, supervisor restart) and the request
method for traffic. A WebSocket upgrade is logged with `status: 101` and `action: "upgrade"`.

## Backend WebSocket sessions

`GET /api/ws/sessions/:id/events?after=&limit=`:

| `type` | Extra fields |
| --- | --- |
| `connecting` | `url` |
| `open` | — |
| `error` | `error`, optional `detail`, `closeCode` |
| `sent` | `opcode` (`text`, `binary`, `ping`, `pong`), `size` |
| `text` | `text` or `base64` + `encoding`, `size`, `truncated` |
| `binary` | `base64`, `size`, `truncated` |
| `frame` | `opcode`, `opcodeId`, `fin`, `rsv1`, `masked`, `length`, `truncated` and a body field |
| `closing` | `code`, `reason` |
| `close` | `code`, `reason`, `clean` |

## Virtual terminal output

Terminal output is not an event log; it is a sequenced chunk stream exposed by both
`GET /api/terminal/sessions/:id/output?after=&limit=` and the terminal WebSocket:

```json
{
  "ok": true,
  "session": { "id": "...", "state": "running", "nextSeq": 42 },
  "chunks": [{ "seq": 41, "data": "base64-bytes" }],
  "firstSeq": 40,
  "nextSeq": 42,
  "truncated": false,
  "missed": false
}
```

`missed: true` means the client cursor is ahead of or older than the retained window; reset the
cursor to 0 (the WebSocket sends `{"type":"reset"}` and the UI clears xterm). A supervised
restart resets the sequence numbers because the new child starts a fresh stream.

## Terminal WebSocket control frames

| Direction | Message |
| --- | --- |
| Server → client | `{"type":"attached","id":"..."}` |
| Server → client | `{"type":"reset"}` |
| Server → client | `{"type":"exit","state":"exited","code":0}` |
| Server → client | `{"type":"error","error":"..."}` |
| Client → server | `{"type":"input","data":"...","encoding":"text\|hex\|base64"}` |
| Client → server | `{"type":"resize","cols":100,"rows":30}` |
| Client → server | `{"type":"ping"}` (server replies `{"type":"pong"}`) |

Output is sent as binary WebSocket frames; terminal input may also be sent as raw binary frames.
