# Security policy

## Reporting a vulnerability

Please do not open a public issue for a security-sensitive report. Use GitHub's private
vulnerability reporting on <https://github.com/Antruly/uvscope/security/advisories/new> or contact
the maintainers through the repository security settings. Include:

- affected version and platform;
- a minimal reproduction;
- impact and any suggested mitigation;
- whether the issue is in uvscope or in a dependency (for example libuvcpp/OpenSSL).

You will receive an acknowledgement as soon as possible. Do not include real credentials,
production certificates or private customer data in a report.

## Supported versions

Security fixes are provided for the latest 1.x release. The 0.x series is no longer supported.

## Threat model

uvscope is a **local developer tool**. Anyone who can authenticate to the workbench can:

- make arbitrary HTTP(S), TCP and UDP requests from the host network;
- start HTTP reverse proxies, TCP/UDP listeners and SOCKS5 servers on local addresses;
- inspect certificates and SOAP contracts;
- use the virtual terminal to run a shell as the daemon user when that module is enabled;
- read stored request history, workspace state and audit records.

Child-process isolation is a **fault-containment boundary**, not a security sandbox: isolated
proxies, listeners, SOCKS5 servers and terminals run as the same OS user with the same filesystem
and network access. Use OS users, `ulimit`, containers or a dedicated host for untrusted
workloads.

Treat access to uvscope as equivalent to code execution with the daemon user's network access.

## Defaults

- Bind address is `127.0.0.1`.
- PBKDF2-HMAC-SHA256, 210,000 iterations.
- Session cookies are `HttpOnly` and `SameSite=Lax`; session tokens are stored as SHA-256 hashes.
- API routes enforce role and module checks server-side.
- Security headers include a restrictive CSP, `X-Frame-Options: DENY` and `nosniff`.
- The TLS client verifies the peer certificate and hostname by default. The explicit
  `insecure` option disables verification for the current request only.
- The response preview is sandboxed and runs no scripts unless “Run scripts” is explicitly
  enabled; remote resources are blocked in the default mode.
- Child processes are spawned from the same executable and communicate over a local IPC pipe;
  there is no network-facing control channel for a child.
- Isolated children inherit the daemon user and environment. Resource limits are the operator's
  responsibility.

## Hardening for shared machines

- Do not run uvscope as root.
- Use a dedicated OS user and a data directory with `0700` permissions.
- Put uvscope behind an authenticating reverse proxy if it must be reachable beyond localhost.
- Disable modules that are not needed.
- Keep the data directory out of backups that leave the machine unless you intend to store session
  and history data there.
- Review reverse proxy targets; the daemon can reach internal networks.

## Dependency vulnerabilities

uvscope pins libuvcpp 1.8.2 (release SDK, plus the minimal WebSocket handshake-header patch kept
in [`patches/upstream/`](patches/upstream/)). The release also publishes an SPDX SBOM, a
`SHA256SUMS` file and a Sigstore keyless signature over the checksums so consumers can audit and
verify what they run. If a dependency advisory applies, open a normal issue for non-sensitive
reports or use private reporting for exploit details.

## Data handling

The data directory contains the SQLite database (users, session hashes, history, workspace,
configuration and audit log), managed certificate private keys and staged downloads. Treat it as
sensitive, restrict its permissions (`0700`), and exclude it from backups that leave the machine
unless that is intended. Request and response bodies are not logged by default.
