# Upstream patches

The patches that used to live here were retired when libuvcpp **v1.8.2** was released:

| Retired patch | Fixed upstream in |
| --- | --- |
| `0001-tls-sni.patch` | v1.8.1 |
| `0002-sync-io-timeouts.patch` | v1.8.1 |
| `0003-install-complete.patch` | v1.8.1 |
| `0004-socks5-proxy-stage.patch` | v1.8.2 (PR #42) |

One patch is currently carried on top of the v1.8.2 release:

| Patch | Topic |
| --- | --- |
| `0005-ws-client-handshake-headers.patch` | `uvcpp_ws_client::set_handshake_header()` so the backend WebSocket debugger can send `Authorization`, `Origin`, `Cookie` and `Sec-WebSocket-Protocol` during the Upgrade request |

uvscope builds against the release SDK (`scripts/build-libuvcpp.sh` clones `v1.8.2` and applies the
patch), and CMake rejects any SDK older than 1.8.2. See
[`docs/LIBUVCPP_UPSTREAM.zh-CN.md`](../../docs/LIBUVCPP_UPSTREAM.zh-CN.md) for the original
reproduction notes.

If a future upstream fix must be carried locally, put a minimal patch in this directory, apply it
from `scripts/build-libuvcpp.sh`, and record the failure and verification alongside it.
