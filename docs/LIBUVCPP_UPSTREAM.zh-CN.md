# libuvcpp 上游问题与 PR 方案

基线：`Antruly/libuvcpp` `v1.8.0`（提交 `13685c1`）。
本文只记录**在当前仓库与机器上复现过**的问题、对应补丁和验证方式。没有复现证据的
内容标为“待验证”，不写成结论。

补丁位于 [`patches/upstream/`](../patches/upstream/)，按文件名顺序应用：

| 补丁 | 主题 | 风险 |
| --- | --- | --- |
| `0001-tls-sni.patch` | TLS SNI；IP 字面量不发 SNI | 低 |
| `0002-sync-io-timeouts.patch` | POSIX 同步 I/O 超时；超时/EOF 区分；失败状态码 | 中 |
| `0003-install-complete.patch` | `cmake --install` 产出可用的 SDK | 低 |
| `0004-socks5-proxy-stage.patch` | 区分"连代理失败"与代理应答后的失败（PR #42，已合并） | 低 |

四个补丁按顺序可以干净应用到 `v1.8.0`（`13685c1`）。当前 `master`
（`48c65bf`）已经包含全部四项等价改动：0001–0003（SNI、POSIX 同步超时、install
完整性）在合并前已进入 master，0004 由 PR
[#42](https://github.com/Antruly/libuvcpp/pull/42) 合并。因此这些补丁只用于
`v1.8.0`；对 master 不应再应用（会冲突或重复）。v1.8.1 早于 0004 的合并，
在下一个 libuvcpp 发布前，uvscope 仍以 “v1.8.0 + 四个补丁” 构建。补丁在 Linux x64
上编译、安装、被一个 `find_package(uvcpp CONFIG)` 的消费者程序链接运行；用这套
SDK 构建的 uvscope 也通过了 `--self-check` 与 `scripts/smoke-test.sh`。

---

## 待上游处理：1.8.2 批次

2026-10-20 按 uvscope 0.3 / 0.4 的需求提交了上游 issue 批次，跟踪页：
[#55](https://github.com/Antruly/libuvcpp/issues/55)。

建议进入 1.8.2：

| Issue | 主题 | uvscope 影响 |
| --- | --- | --- |
| [#44](https://github.com/Antruly/libuvcpp/issues/44) | 域名解析 `AF_UNSPEC` / IPv6 | 所有出站；IPv6-only 目标 |
| [#45](https://github.com/Antruly/libuvcpp/issues/45) | peer 证书链与校验错误 | 0.4 TLS 实时证书检查 |
| [#46](https://github.com/Antruly/libuvcpp/issues/46) | SOCKS5 会话结束 / 字节数 | 0.3 服务端页面完整性 |
| [#47](https://github.com/Antruly/libuvcpp/issues/47) | 客户端 br 解压 / 阈值 | 0.4 body 解码 |
| [#48](https://github.com/Antruly/libuvcpp/issues/48) | 同步 `*_wait` 总 deadline | 0.3 `timeoutMs` 契约 |
| [#49](https://github.com/Antruly/libuvcpp/issues/49) | UDP 域名解析 / IPv6 connect | 0.3 UDP 探测 |
| [#54](https://github.com/Antruly/libuvcpp/issues/54) | ws client 的 SOCKS5 阶段 | 0.4 WS 代理错误分类 |

后续版本：#50 WebSocket 逐帧接口、#51 HTTP 客户端流式响应、#52 HTTP/2 帧事件、
#53 HTTP/3 / QUIC 能力清单。1.8.2 发布后，uvscope 的 0001–0004 补丁应可全部删除。

---

## 1. TLS SNI 缺失（P0，已修复）

### 现象

访问任何依赖 SNI 的 HTTPS 虚拟主机时，要么握手失败，要么拿到该 IP 的默认证书，
证书主机名校验随即失败。`uvscope` 0.1.0 的所有公网 HTTPS 调试都受影响。

### 根因

`uvcpp_ssl` 没有暴露 `SSL_set_tlsext_host_name()`，两条客户端路径都没有发送
`server_name`：

- 异步 memory-BIO 路径：`src/net/uvcpp_tcp_client.cpp` 的
  `apply_tls_hostname()`（v1.8.0 约 3263 行）；
- 同步阻塞路径：`src/web/uvcpp_http_client.cpp` 的
  `do_ssl_handshake()`（约 1091 行）。

同步路径自己 `new uvcpp_ssl(ctx, fd)`，不经过 `uvcpp_tcp_client::connect()`，
所以两条路必须各接一次，不能只在其中一处修。

### 补丁

`0001-tls-sni.patch`：

- 新增 `uvcpp_ssl::set_server_name(const char*, size_t)`；
- 两条路径都在握手前调用；
- 按 RFC 6066 §3，**IP 字面量不发送 SNI**（`uv_ip4_addr` / `uv_ip6_addr` 判定），
  避免部分服务端拒绝带 IP 的 `server_name`；证书校验仍按原主机名进行。

### 验证

本地 TLS 服务端用 `SSLContext.sni_callback` 记录 `server_name`：

```text
async localhost   -> SNI=localhost
async 127.0.0.1   -> SNI=None
sync  localhost   -> SNI=localhost
sync  127.0.0.1   -> SNI=None
```

---

## 2. 同步 `*_wait` 在 POSIX 上忽略 `timeout_ms`（P0，已修复）

### 现象

`connect_wait()` / `send_wait()` / `send_wait_ssl()` 的 `timeout_ms` 只在 Windows
生效。Linux/macOS 上：

- 服务端收下请求后不响应：客户端一直等到对端关闭，然后把**空响应当成功**返回；
- TCP 已连接但对端不开始 TLS 握手：`do_ssl_handshake()` 无限阻塞。

这不是理论问题：`uvscope` 的稳定路径正是同步 HTTP/1.1，`connect_wait/send_wait`
的返回值被直接写进调试结果。

### 复现（修复前，`timeout_ms = 1000`）

服务端收下请求后静默 4 秒再关闭：

```text
plain: RESULT rc=0 elapsed_ms=4000 status=200
tls:   RESULT rc=0 elapsed_ms=4000 status=200
```

`timeout_ms` 被完全忽略；超时还被交付成 `200 OK`。

服务端只 accept、不做 TLS 握手，客户端 `connect_wait(..., 3000)`：

```text
$ timeout 6 ./timeout_test tls localhost <port>
exit=124          # 6 秒后仍在握手里
```

### 根因

1. `send_wait_ssl()` / `send_wait_plain()` 里设置 `SO_RCVTIMEO` / `SO_SNDTIMEO`
   的代码整段包在 `#ifdef _WIN32` 里；注释声称 POSIX 没有等价写法，但
   `SO_RCVTIMEO` / `SO_SNDTIMEO` 在 Linux 和 macOS 上都可用。
2. `read_one_message()` 把读取返回的 `n <= 0` 一律当 EOF。即使超时真的发生，
   半条响应也会被当成完整响应。
3. `uvcpp_ssl::read()` 用 `0` 同时表示 `close_notify` 和 `WANT_READ`；同步路径
   无法区分“对端正常关闭”和“超时”。
4. 握手失败后用一个 `for (i < 4)` 的紧循环重试；在阻塞 socket 上每次重试都会
   再等满一个 `timeout_ms`，把 3 秒超时放大成 12 秒。
5. 失败时 `uvcpp_http_response` 保留默认的 `200 OK`，调用方日志里会出现
   “请求失败，但状态码 200”。

### 补丁

`0002-sync-io-timeouts.patch`：

- 两个平台都安装 socket 收发超时；
- `EAGAIN` / `EWOULDBLOCK` / `ETIMEDOUT` 映射为 `UV_ETIMEDOUT`；
- `read_one_message()` 返回负的 libuv 错误码，不再把错误当 EOF；
- TLS 读取用 `last_ssl_error() == SSL_ERROR_ZERO_RETURN` 区分正常关闭；
- 删除握手重试循环（阻塞 socket 上 0 就是超时）；
- 同步失败路径把 `resp.status_code` 复位为 `HTTP_STATUS_NONE`（0）。

### 验证（修复后）

```text
plain silent:  RESULT rc=-110 elapsed_ms=1024 status=0
tls   silent:  RESULT rc=-110 elapsed_ms=1037 status=0
handshake stall: connect_wait rc=-110
normal TLS:      RESULT rc=0 elapsed_ms=0 status=200
```

`-110` 是 libuv 的 `UV_ETIMEDOUT`。

### 残留

`SO_RCVTIMEO` 是**每次 I/O** 的超时，不是整次请求的总 deadline。对端每隔
`timeout_ms - ε` 发一个字节，仍可以把请求拖住。后续可以把
`timeout_ms` 换成单调时钟 deadline，并在每次读写前用 `poll()` 截断等待时间。
这一点不阻塞当前补丁。

---

## 3. `cmake --install` 产出的前缀不可用（P1，已修复）

### 现象

`package_release.py` 手工组装的 release zip 可以使用，但从源码
`cmake --install --prefix X` 出来的前缀不行：

- Linux：`libuvcpp.so` 的 `DT_NEEDED` 里有 `libllhttp.so.9.2`，前缀里没有；
  `ldd` 显示 `not found`；
- Windows MinGW：`libuvcpp.dll` 的导入表依赖 `libllhttp.dll`，前缀 `bin/`
  里没有，程序以 `0xC0000135`（`STATUS_DLL_NOT_FOUND`）静默退出；
- 安装后的共享库没有 RPATH，即使把依赖手工拷到同一个 `lib/` 也找不到；
- 用 FetchContent 编 libuv 时，头装在 `include/libuv/`，但公开头写的是
  `#include <uv.h>`，导出的 CMake target 没有把这个目录传给消费者。

`uvscope` 的 `scripts/build-libuvcpp.sh` 之所以手工拷贝 `libllhttp.dll`，
就是在绕开这条 install 规则。

### 补丁

`0003-install-complete.patch`：

- 安装 FetchContent 依赖里所有 `SHARED_LIBRARY` target（llhttp、zlib、libuv），
  `install(TARGETS ...)` 会一并生成 SONAME 符号链接；
- 给 `uvcpp` / `uvcpp_s` 设置 `INSTALL_RPATH`：
  Linux `$ORIGIN`，macOS `@loader_path`；
- 只在**自己编译 libuv** 时把 `include/libuv` 写进
  `INTERFACE_INCLUDE_DIRECTORIES`（系统 libuv 不安装这个目录，写了会让
  `find_package` 在 generate 阶段报 “includes non-existent path”）；
- 同一配置下在 `uvcpp.pc` 的 `Cflags` 里补 `-I${includedir}/libuv`。

### 验证

```text
$ cmake --install build --prefix /tmp/uvcpp-install-pr
$ ls /tmp/uvcpp-install-pr/lib
libllhttp.so  libllhttp.so.9.2  libllhttp.so.9.2.0  libuvcpp.so  cmake  pkgconfig

$ readelf -d /tmp/uvcpp-install-pr/lib/libuvcpp.so | grep RUNPATH
Library runpath: [$ORIGIN]

$ ldd /tmp/uvcpp-install-pr/lib/libuvcpp.so | grep llhttp
libllhttp.so.9.2 => /tmp/uvcpp-install-pr/lib/libllhttp.so.9.2
```

消费者：

```cmake
find_package(uvcpp CONFIG REQUIRED)
target_link_libraries(consumer PRIVATE uvcpp::uvcpp)
```

```text
$ cmake -S consumer -B build -DCMAKE_PREFIX_PATH=/tmp/uvcpp-install-pr
$ cmake --build build
$ LD_LIBRARY_PATH=/tmp/uvcpp-install-pr/lib ./build/consumer
uv=1.48.0 web=1
```

### 建议一并加的 CI

在 CI 里增加一个 `install + consumer` 冒烟格：安装到临时前缀，配置并运行一个
只包含 `<uvcpp.h>`、`<net/uvcpp_tcp_client.h>` 的消费者。这样 install 规则和
`package_release.py` 两条路径不会再次漂移。

---

## 3.1 SOCKS5 失败阶段不可区分（P1，已修复）

### 现象

配置 SOCKS5 代理后，`socks5_last_error()` 无法区分失败发生在哪一侧：

- 代理端口没人监听：`uv_strerror(ECONNREFUSED)` → `connection refused`；
- 代理已应答并拒绝 CONNECT：`reply_string(CONNECTION_REFUSED)` 也是
  `connection refused`。

`uvscope` 需要把错误分类为“代理不可达”和“目标拒绝”，两者不能混为一谈。

### 补丁

`0004-socks5-proxy-stage.patch`（上游 PR
[#42](https://github.com/Antruly/libuvcpp/pull/42)，已合并）：

- `uvcpp_tcp_client` 新增只读 `socks5_proxy_connect_failed()`；在异步/同步
  的代理 TCP 连接、DNS 解析与连接超时路径置位，进入握手阶段前清零；
- `uvcpp_http_client` 转发同名访问器。

### 验证

`uvscope` 的 `scripts/smoke-test.sh` 对三类失败做断言：

```text
代理端口关闭      -> proxy.proxy_unreachable
代理应答后目标关闭 -> proxy.target_rejected
错误密码          -> proxy.auth_failed
```

同一目标经代理成功时，TCP / HTTP / UDP 三条客户端路径都能拿到目标响应。

---

## 4. 主机名解析只取 IPv4（P1，待实现）

### 事实

两条解析路径都写死 `hints.ai_family = AF_INET`：

- 异步：`src/net/uvcpp_tcp_client.cpp` `connect_transport()` 的
  `getaddrinfo` 回调（v1.8.0 约 884 行）；
- 同步：同文件 `resolve_host_sync()`（约 1624 行）。

数字 IPv6 字面量有快速路径，但**域名解析不会得到 AAAA**。IPv6-only 主机直接
失败；双栈主机只会走 A。

### 建议方案

分两步，避免一次改动引入 Happy Eyeballs 的复杂度：

1. `AF_UNSPEC` + 依次尝试返回的地址。异步路径需要为每次尝试准备一个新的
   `uv_tcp_t`（同一个 handle 在 connect 失败后重连的行为在不同 libuv 版本上
   不一致，不能假设）；同步路径把 `resolve_host_sync()` 改成返回地址列表，
   `connect_wait()` 逐个尝试。
2. 再做 250ms 的 Happy Eyeballs（RFC 8305）并发/延迟竞速。没有第 1 步的
   逐个尝试，不建议直接开 `AF_UNSPEC`：部分网络会先拿到 AAAA，但实际只有
   IPv4 出口。

在完成 1 之前，不建议只把 `AF_INET` 改成 `AF_UNSPEC` 就发布。

---

## 5. 关于“异步 TLS / h2 不可用”的更正（已复测）

0.1.0 的开发记录里写过“异步 TLS 的 ALPN 为空、`send()` 返回 `UV_EINVAL`”。
在 `v1.8.0 + SNI` 上重新复测，**没有复现**：

- 用 `-DUVCPP_ENABLE_NGHTTP2=ON` 构建 libuvcpp；
- 本地 TLS 服务端 `set_alpn_protocols(['h2'])`；
- `set_http2_enabled(true)` → `connect()` → `send()`：

```text
connect cb status=0 alpn=h2
send rc=0
server FIRSTBYTES= 505249202a20485454502f322e300d0a0d0a534d0d0a0d0a
                    ("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")
```

ALPN 协商为 `h2`，`send()` 返回 0，h2 客户端 preface 正常发出。服务端直接断开
时回调拿到 `-125`（`UV_ECANCELED`），这是预期路径。

`UV_EINVAL` 的真实来源是 `on_tcp_data()` 里的
`parser_->has_error()`：HTTP/1.1 解析器收到了非 HTTP 字节。它对应的场景是
**ALPN 没协商出 h2，但对端仍在发 h2 帧**，属于对端协议错误或旧版客户端没发
SNI/ALPN，而不是“异步 TLS 一律不可用”。旧结论应撤回。

仍然成立的风险：

- HTTP/3 / QUIC 路径本次没有构建和复测；
- 没有在 Windows MinGW、macOS 和真实公网服务上跑同一套 ALPN 矩阵；
- 发布构建仍然关闭 `NGHTTP2` / `HTTP3`，在补齐跨平台 CI 之前不建议打开。

---

## 6. 优化项（不阻塞）

| 项 | 现状 | 建议 |
| --- | --- | --- |
| 依赖构建时间 | 每个平台/每个 CI job 都从源码编 libuv、llhttp、zlib、nghttp2 | CI 缓存 patched libuvcpp，或用 `FETCHCONTENT_SOURCE_DIR_*` 指向缓存仓库 |
| 发布与安装漂移 | `package_release.py` 手工组装，`cmake --install` 是另一条路 | 加 install+consumer CI 格（见第 3 节） |
| 大块 SSL 写入 | `uvcpp_ssl::write/read` 把 `size_t` 强转 `int` | 超过 `INT_MAX` 时分块；当前 HTTP 请求不会触发，属防御性修复 |
| 同步路径总 deadline | 只有每次 I/O 超时 | 单调时钟 deadline + `poll()`（见第 2 节残留） |
| 错误响应状态码 | 已在 0002 修复为 `HTTP_STATUS_NONE` | 补一条断言，锁定“失败不得返回 200” |

---

## 7. 提交 PR 的建议顺序

每个补丁一个 PR，不要合并成一个大 PR：

1. `fix(tls): send SNI for DNS host names` — 0001（master 已包含；PR #38 仍为 open）
2. `fix(web): honor timeout_ms in blocking *_wait paths` — 0002（master 已包含；PR #39 仍为 open）
3. `fix(build): make cmake --install produce a usable SDK` — 0003（已合并，PR #40）
4. `fix(net): expose the SOCKS5 proxy-connect stage` — 0004（PR #42，已合并）

建议的验证命令（每个 PR 都附上）：

```sh
git clone --depth 1 --branch v1.8.0 https://github.com/Antruly/libuvcpp.git
cd libuvcpp
git apply /path/to/000x-*.patch
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_BUILD_WEB=ON -DUVCPP_BUILD_WEBAPP=ON -DUVCPP_BUILD_NET=ON \
  -DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_ENABLE_ZLIB=ON \
  -DUVCPP_ENABLE_NGHTTP2=OFF -DUVCPP_ENABLE_QUIC=OFF -DUVCPP_ENABLE_HTTP3=OFF \
  -DUVCPP_BUILD_TESTS=OFF
cmake --build build -j
cmake --install build --prefix /tmp/uvcpp-prefix
```

0002 的最小回归用例（本地静默 TLS 服务端 + `send_wait(..., 1000)`）：

- 服务端不响应：`rc == UV_ETIMEDOUT`，耗时约 1 秒，`resp.status_code == 0`；
- 服务端正常响应：`rc == 0`，`resp.status_code == 200`；
- 服务端只 accept 不握手：`connect_wait(..., 3000)` 在约 3 秒返回
  `UV_ETIMEDOUT`，不得无限阻塞。

0003 的最小回归用例：安装到临时前缀后，配置并运行一个
`find_package(uvcpp CONFIG)` 消费者；Linux 上 `ldd` 不得出现 `not found`。
