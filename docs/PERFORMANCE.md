# Performance baselines

uvscope ships a dependency-free reference benchmark for the reverse proxy. The goal is a
repeatable **relative** measurement on one machine — not a cross-machine guarantee. Absolute
numbers depend on the CPU, kernel, Python client and the origin server.

## What it measures

`scripts/bench.sh` starts:

1. a local HTTP/1.1 origin (Python `ThreadingHTTPServer`, Nagle disabled) on `--origin-port`;
2. a throwaway uvscope daemon on `--port` with a temporary data directory;
3. a reverse proxy on `--proxy-port` targeting the origin (in-process by default, or a child
   process with `--isolated`).

It then sends `--requests` sequential keep-alive `GET` requests to the origin directly and through
the proxy, and reports throughput plus mean/p50/p95/max latency. Both paths use the same Python
`http.client` connection and payload, so the difference is the proxy path itself.

```sh
./scripts/bench.sh ./build/uvscope --requests 500
./scripts/bench.sh ./build/uvscope --requests 500 --isolated
./scripts/bench.sh ./build/uvscope --requests 500 --json
```

The client is single-threaded: this is a latency/overhead measurement, not a saturation test. For
concurrency or saturation numbers, drive the listener with your own load generator and compare
release builds on the same host.

## Reference run

Recorded on 2026-10-25 with the v1.0.0 release build:

- CPU / host: `Linux 6.14.0-37-generic x86_64` (glibc 2.39), development container
- Proxy: in-process reverse proxy, empty rule set
- Requests: 500, warmup 50, keep-alive, sequential

| Path | req/s | mean ms | p50 ms | p95 ms | max ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Direct origin | 6209 | 0.16 | 0.15 | 0.23 | 0.34 |
| Reverse proxy (in-process) | 567 | 1.76 | 1.76 | 1.88 | 2.59 |
| Reverse proxy (isolated child) | 577 | 1.73 | 1.73 | 1.84 | 2.88 |

Interpretation:

- The proxy adds about **1.6 ms** per sequential request on this host.
- In-process and isolated proxies have effectively the same data path cost; isolation adds a
  child process and IPC telemetry, not per-request IPC. Choose isolation for fault containment,
  not for throughput.
- The direct-origin number is dominated by the Python client and loopback; use the ratio, not the
  absolute rate.

Record the command, host and commit alongside any baseline you publish. Numbers from different
hosts or Python versions are not comparable.

## Release verification

The release pipeline runs, per platform:

- `--self-check` (IPC, crypto, SQLite, CA, HTTP server, proxy-child);
- the full smoke suite on Linux;
- a Windows ConPTY terminal smoke and the H2 runtime check;
- a macOS self-check and the H2 runtime check.

The benchmark is a developer/operator tool; it is not a pass/fail CI gate because CI hosts are
shared and noisy.
