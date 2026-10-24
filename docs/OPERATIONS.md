# Operations

uvscope is a single-binary daemon with a SQLite database and a static UI bundle. This page covers
day-to-day operation, backup, upgrades and hardening.

## Data directory

The daemon prints the data directory at startup. Override it with `--data-dir` or
`UVSCOPE_DATA_DIR`; the default is `~/.local/share/uvscope` on Linux and the platform equivalent
elsewhere.

| Path | Contents |
| --- | --- |
| `uvscope.db` | SQLite database: users, sessions, settings, history, workspace, proxied configuration, audit log |
| `certs/` | Managed TLS certificates and private keys (created by the certificate vault) |
| `downloads/` | Staged large responses and resumable partial files |
| `backup-<id>.db` | Temporary snapshot used by the backup endpoint |

The data directory is the entire persistent state. Child processes hold no state of their own;
their configuration is reconstructed from the parent and the database.

## First run

1. Start `uvscope --port 8787` (add `--open` to launch a browser).
2. Open <http://127.0.0.1:8787>; the first visit creates the administrator account.
3. Enable the modules you need in **Settings → Modules**. The terminal is off by default.
4. Add users under **Users**; roles are `admin`, `operator` and `viewer`.

`GET /api/health` is public and returns the version, `libuvcpp` version, uptime, setup state and
transport capabilities. Use it for supervision and smoke checks. It is not an authenticated
endpoint and exposes no user data.

## Users, roles and sessions

- `admin` — full access, including users, settings, reset and the supervisor.
- `operator` — debug modules, gateways, listeners and terminals.
- `viewer` — read-only access to dashboards and history surfaces.

Sessions are server-side with a seven-day expiry. `GET /api/auth/sessions` lists active sessions;
`DELETE /api/auth/sessions/:id` revokes one. Changing a password invalidates that user's other
sessions.

## Modules

Module switches live in `PUT /api/settings` (`{"modules": {"terminal": true}}`). Disabling the
terminal closes all terminal sessions and unregisters their supervisor entries. Disabling other
modules blocks new API calls but does not stop running long-lived services; stop those explicitly
in their views.

## Backup and restore

- `GET /api/backup` returns a consistent SQLite snapshot as a download.
- `POST /api/backup/restore` stages an uploaded database as `uvscope.db.restore` and applies it on
  the next start after validation.
- To back up manually while the daemon is stopped, copy `uvscope.db` (plus `certs/` if you want to
  keep private keys). Back up `downloads/` only if you need staged files.

Stop the daemon or use the API snapshot; copying a live SQLite file without the API can capture a
partial write.

## Reset

`POST /api/reset` with `{"module": "all"}` stops proxies, listeners, SOCKS5 servers and terminals,
unregisters their supervisor entries, removes the database, recreates the schema and returns
`setupRequired: true`. Module-scoped resets (`proxy`, `terminal`, `tcpudp`, `http`, `websocket`,
`soap`, `tls`) clear that module's configuration.

## Long-lived services and isolation

Reverse proxies, TCP/UDP listeners, SOCKS5 servers and virtual terminals accept
`isolated: true`. Isolated services run in a child process of the same binary:

- the parent caches telemetry and proxies commands over IPC;
- a crash is restarted with exponential backoff (2–60 s) and a new PID;
- an explicit stop clears the desired state, so the watchdog leaves the service down;
- deleting a service unregisters it before the child is stopped and reaped;
- disabling the terminal module unregisters and closes all terminal sessions.

Isolation is a fault boundary, not a security sandbox: children run as the same OS user with the
same filesystem and network access. Resource limits (`ulimit`, containers, a dedicated OS user)
must be applied by the operator.

`GET /api/supervisor` and `GET /api/supervisor/events` show desired/running state, restart counts
and the event history. Terminal sessions do not survive a daemon restart; proxies, listeners and
SOCKS5 servers with `autoStart` are restored from SQLite.

## Logs

uvscope logs to stderr. Run it under your service manager to capture output:

```sh
uvscope --port 8787 >>/var/log/uvscope.log 2>&1
```

Per-service telemetry is not written to the log; use the API views (`/api/proxies/:id/logs`,
listener/SOCKS5 events, terminal output) and the audit log in the UI. Request bodies are not
logged.

## Upgrades

1. Stop the daemon.
2. Back up the data directory (or use `GET /api/backup` while running).
3. Replace the binary and the `share/uvscope/web` bundle with the new release.
4. Start the daemon; pending SQLite migrations apply automatically.

Version rollback across a schema migration is not supported; restore the pre-upgrade database if
you need to go back.

## Hardening for shared machines

- Do not run as root. Use a dedicated OS user and a data directory with `0700` permissions.
- Keep the default `127.0.0.1` bind unless the service is behind an authenticating proxy.
- Serve the UI over TLS (`--tls-cert` / `--tls-key`) if it is reachable beyond localhost.
- Disable modules you do not use; keep the terminal off unless needed. A terminal grants a shell
  as the daemon user.
- Isolated services still share the OS user; use OS-level limits for untrusted workloads.
- Review reverse proxy targets and listener bind addresses; the daemon can reach internal networks.
- Treat the data directory as sensitive: it contains session hashes, history and private keys.

See [SECURITY.md](https://github.com/Antruly/uvscope/blob/main/SECURITY.md) for the threat model
and private vulnerability reporting.
