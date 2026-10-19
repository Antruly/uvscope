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

## Threat model

uvscope is a **local developer tool**. Anyone who can authenticate to the workbench can:

- make arbitrary HTTP(S), TCP and UDP requests from the host network;
- start HTTP reverse proxies on local ports;
- inspect certificates and SOAP contracts;
- read stored request history.

Treat access to uvscope as equivalent to code execution with the daemon user's network access.

## Defaults

- Bind address is `127.0.0.1`.
- PBKDF2-HMAC-SHA256, 210,000 iterations.
- Session cookies are `HttpOnly` and `SameSite=Lax`; session tokens are stored as SHA-256 hashes.
- API routes enforce role and module checks server-side.
- Security headers include a restrictive CSP, `X-Frame-Options: DENY` and `nosniff`.
- The TLS client verifies the peer certificate and hostname by default. The explicit
  `insecure` option disables verification for the current request only.

## Hardening for shared machines

- Do not run uvscope as root.
- Use a dedicated OS user and a data directory with `0700` permissions.
- Put uvscope behind an authenticating reverse proxy if it must be reachable beyond localhost.
- Disable modules that are not needed.
- Keep the data directory out of backups that leave the machine unless you intend to store session
  and history data there.
- Review reverse proxy targets; the daemon can reach internal networks.

## Dependency vulnerabilities

uvscope pins libuvcpp 1.8.0 and carries a minimal SNI patch. If a dependency advisory applies,
open a normal issue for non-sensitive reports or use private reporting for exploit details.
