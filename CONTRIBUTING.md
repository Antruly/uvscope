# Contributing to uvscope

Thanks for helping build the protocol workbench. Small, verifiable changes are preferred over large
rewrites.

## Workflow

1. Open an issue describing the protocol behavior, UI problem or platform gap.
2. Fork and branch from `main`.
3. Keep one concern per pull request.
4. Build and run the checks below.
5. Describe the exact command and output used to verify the change.

## Build and checks

```sh
./scripts/build.sh
./build/uvscope --self-check
npm --prefix web run typecheck
npm --prefix web run build
```

For UI changes, include a screenshot at 1600×1000 (and a narrow viewport if relevant).

## Code guidelines

- C++17, RAII, no blocking I/O on the main event loop. Short probes go through the libuv thread
  pool; long-lived services get their own loop/process boundary.
- Keep API errors in the `{ ok, error, code }` shape.
- New modules need:
  - a module id and a default toggle;
  - a server-side permission check;
  - a navigation entry;
  - a safe empty state and a failure state;
  - an entry in the development plan.
- Do not invent protocol data. If a transport does not expose a field, return `null`/omit it and
  document the limitation.
- Do not add a second networking stack. All target connections go through libuvcpp. If a capability
  is missing, fix or patch libuvcpp and record the upstream reference.

## libuvcpp changes

Changes to libuvcpp live in a patch under `patches/` and must include:

- a focused explanation of the failure;
- the smallest possible code change;
- a reproducible test or a precise manual verification;
- a note for the upstream project.

The current SNI patch exists because the HTTP client did not set `SSL_set_tlsext_host_name()`, so
modern virtual hosts either abort or serve a default certificate.

## Security

Do not include real credentials, private keys or customer data in issues, tests or screenshots.
See [SECURITY.md](SECURITY.md) for private reporting.

## License

By contributing, you agree that your contribution is licensed under the MIT License.
