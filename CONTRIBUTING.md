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
npm --prefix web run check:i18n
bash ./scripts/smoke-test.sh ./build/uvscope 18787 18788
bash ./scripts/h2-smoke.sh ./build/uvscope 18820 18821
python3 -m pip install mkdocs-material && mkdocs build
```

`smoke-test.sh` needs `curl`, `node`, `openssl` and `python3` for the full matrix. For UI changes,
include a screenshot at 1600×1000 (and a narrow viewport if relevant). Follow the
[Code of Conduct](CODE_OF_CONDUCT.md).

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

uvscope builds against the libuvcpp **v1.8.2** release SDK. One patch is currently carried on top
of that tag: `patches/upstream/0005-ws-client-handshake-headers.patch`, which adds
`uvcpp_ws_client::set_handshake_header()` so the backend WebSocket debugger can send
`Authorization`, `Origin`, `Cookie` and `Sec-WebSocket-Protocol` during the Upgrade request. The
earlier SNI, POSIX timeout, install-completeness and SOCKS5 stage patches were retired in
1.8.1/1.8.2; see [docs/LIBUVCPP_UPSTREAM.zh-CN.md](docs/LIBUVCPP_UPSTREAM.zh-CN.md).

If a new patch is needed, it must include:

- a focused explanation of the failure;
- the smallest possible code change;
- a reproducible test or a precise manual verification;
- a note for the upstream project.

Patch files are applied in filename order by `scripts/build-libuvcpp.sh`; a patch that no longer
applies must be updated or dropped when the SDK tag moves.

## Security

Do not include real credentials, private keys or customer data in issues, tests or screenshots.
See [SECURITY.md](SECURITY.md) for private reporting. Event schemas and the public API are
documented in [docs/EVENTS.md](docs/EVENTS.md) and [docs/API.md](docs/API.md); additive changes
are expected within 1.x.

## License

By contributing, you agree that your contribution is licensed under the MIT License.
