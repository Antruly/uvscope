# Agent guide

This repository is designed for incremental, verifiable agent work. Read
`docs/DEVELOPMENT_PLAN.zh-CN.md` and `docs/ARCHITECTURE.md` before changing protocol behavior.

## Ground rules

- One concern per change. Do not combine protocol, database, UI and packaging work in one step.
- Every change must have a command that proves it. Examples:
  - `./scripts/build.sh`
  - `./build/uvscope --self-check`
  - `bash ./scripts/smoke-test.sh ./build/uvscope 18787 18788`
  - `bash ./scripts/h2-smoke.sh ./build/uvscope 18820 18821`
  - `npm --prefix web run build`
  - `mkdocs build`
- No second networking stack. All target I/O goes through libuvcpp.
- Never fabricate protocol fields. Missing data is omitted or explicitly marked unknown.
- Keep `README.md`, `docs/API.md`, `docs/EVENTS.md` and the help page in sync with behavior.
  Event fields may be added in 1.x but not renamed or retyped.
- Do not include credentials, certificates, API keys or traffic captures in commits.

## Module template

A new debug module needs:

1. A server route under `/api/...` that calls `Auth::require_module`.
2. A stable JSON result with `ok`, a typed error and timing fields.
3. A module default in `Store::module_states` / the settings UI.
4. A router entry and a cached tab-ready Vue view.
5. A short section in `docs/API.md` and the help page.
6. A smoke assertion if the module runs without external services.

## Windows

Build in MSYS2 MINGW64. Do not use MSVC for the default release toolchain. When running a freshly
built `uvscope.exe` from a shell, put the SDK/toolchain `bin` directories on `PATH` or copy the
dependent DLLs next to the executable.

## libuvcpp changes

Put the patch under `patches/`, keep it minimal, and record the failure and verification. The
patches retired with libuvcpp 1.8.2 in `docs/LIBUVCPP_UPSTREAM.zh-CN.md` are the reference format.

## Release checklist

1. `npm --prefix web ci && npm --prefix web run check:i18n && npm --prefix web run build`
2. `./scripts/build.sh`
3. `./build/uvscope --self-check`
4. `bash ./scripts/smoke-test.sh ./build/uvscope 18787 18788`
5. `bash ./scripts/h2-smoke.sh ./build/uvscope 18820 18821`
6. `mkdocs build` and `./scripts/bench.sh ./build/uvscope` (record the reference run)
7. CI green on Linux x64, Windows x64 MinGW and macOS arm64
8. Update the version in `CMakeLists.txt`, `web/package.json`, `web/package-lock.json`,
   `web/src/stores/auth.ts`, `packaging/windows/uvscope.iss` and the docs
9. Update `CHANGELOG.md` and tag `v<version>`
10. Verify release assets: Linux x64/arm64 tar/deb/rpm, Windows zip/setup.exe, macOS tar.gz,
    `SHA256SUMS`, `SHA256SUMS.sigstore.json` and the SPDX SBOM
11. Verify the published Linux artifact with `--version` and `--self-check`

Windows arm64, HTTP/3 and native plugin loading are not part of the 1.x release matrix.
