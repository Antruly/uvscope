# uvscope 开发计划

> 版本：0.3.0 发布收尾
> 技术底座：libuvcpp 1.8.2 + Vue 3 + SQLite + libuvcpp WSDL/SOAP  
> 目标：做一款覆盖 Web 调试、协议探测与可编程反向代理的本地瑞士军刀，并保持单机、可审计、可离线部署。

---

## 1. 当前基线（0.1.0 已完成）

### 1.1 已交付

- 单进程 C++17 守护进程，使用 libuvcpp webapp 提供 HTTP API 与前端静态资源。
- Vue 3 + Vite + Pinia + Vue Router 前端：
  - 可收起左侧菜单；
  - 多标签工作区与 KeepAlive 缓存；
  - 概览、HTTP、WebSocket、TCP/UDP、SOAP/WSDL、TLS、反向代理、用户、设置、关于、帮助。
- SQLite 持久化：
  - 首次启动管理员初始化；
  - 用户、角色（admin/operator/viewer）、服务端会话、设置、请求历史、代理配置；
  - WAL 模式，主进程单写者。
- 安全：
  - PBKDF2-HMAC-SHA256（210k 次）密码哈希；
  - HttpOnly / SameSite=Lax Cookie；
  - 会话令牌仅存 SHA-256；
  - 路由级权限检查与安全响应头。
- HTTP/1.1 与 HTTPS 请求调试：
  - 任意方法、Header、Body、超时、压缩、不安全 TLS 开关；
  - 状态码、响应头、原始 Body、耗时、大小与历史记录。
- WebSocket 浏览器端调试：文本/十六进制帧、实时日志、帧计数。
- TCP / UDP 一次性探测：文本/十六进制载荷、超时、原始字节与耗时。
- TLS 证书解析：Subject / Issuer / SAN / KeyUsage / EKU / 指纹 / 有效期。
- SOAP/WSDL：WSDL 抓取或粘贴、operation 识别、SOAPAction、Envelope 生成、Fault 展示。
- HTTP 反向代理：
  - 多实例、端口监听、上游转发；
  - TLS 监听证书路径；
  - 实时请求日志、启停、编辑、删除；
  - 代理与主服务共享 libuvcpp 技术栈。
- 开源运营能力：关于页、GitHub 链接、star 数、版本检查、更新说明、帮助页。
- 跨平台构建基础：CMake、MinGW-w64、CPack、GitHub Actions 工作流、Windows Inno Setup 安装脚本。
- libuvcpp 上游修复：TLS SNI、同步 I/O 超时、install 完整性与 SOCKS5 失败阶段；这些改动
  已在 libuvcpp v1.8.1 / v1.8.2 发布，uvscope 现直接使用 1.8.2 发布版 SDK，不再携带本地补丁。
  历史复现记录见 [`LIBUVCPP_UPSTREAM.zh-CN.md`](LIBUVCPP_UPSTREAM.zh-CN.md)。

### 1.2 0.1.0 明确不承诺的能力

- HTTP/2 与 HTTP/3 目前只保留协议偏好与事件字段，libuvcpp 异步 TLS/HTTP2/HTTP3 路径尚未完成跨平台验证，生产调试请使用 HTTP/1.1。
- 反向代理仅支持 HTTP/1.1 转发；WebSocket Upgrade、HTTP/2 入站、按路径/Host 规则匹配属于下一阶段。
- TCP/UDP 仅一次性探测，没有常驻监听与动态会话。
- 没有原始 QUIC 双向流 / datagram 调试。

### 1.3 本轮新增范围（2026-10-20）

| 优先级 | 能力 | 说明 |
| --- | --- | --- |
| P0 | 响应预览 | HTML 沙箱渲染、图片、音频、视频、PDF；不能预览的类型保持 raw/hex |
| P0 | 国际化 | 浏览器语言自动选择，中文/英文手动切换并持久化 |
| P0 | 主题 | 亮色 / 暗色 / 跟随系统；补齐硬编码颜色 |
| P1 | 持久 TCP 客户端 | 一条连接多次发送、持续接收、十六进制视图、主动关闭 |
| P1 | TCP 服务端 | 监听、会话列表、逐会话发送/关闭、日志与字节统计 |
| P1 | UDP 服务端 | 绑定、收包、按来源回复、会话时间线 |
| P1 | SOCKS5 客户端 | HTTP / TCP / UDP 出站可选择代理；认证与错误分类 |
| P1 | SOCKS5 服务端 | CONNECT / BIND / UDP ASSOCIATE；no-auth 与用户名密码 |
| P2 | QUIC / HTTP/3 | 客户端与 stream/datagram 调试，跨平台验证后再打开 |

约束：

- 每个能力单独一个切片、单独验证，不把协议、数据库、UI、打包混在一次改动里。
- 所有目标网络 I/O 继续只走 libuvcpp；不引入第二个网络栈。
- SOCKS5 “没连上代理”必须报错，**绝不静默直连**。
- 预览只在沙箱里渲染；默认禁脚本、禁远程资源，明确写出安全边界。

---

## 2. 架构原则

1. **单网络栈**：所有出站网络请求都经过 libuvcpp；不引入 libcurl、Boost.Asio、Node.js 网络栈。
2. **主进程单写 SQLite**：代理和协议子运行时只读配置快照；所有写入由主进程完成。
3. **前端不直接触达目标 HTTP 服务**：HTTP/TCP/UDP/TLS/SOAP 都走 `/api/*`，便于统一鉴权、审计与历史。
4. **模块隔离**：每个功能域有独立 API、权限、模块开关和错误边界；一个模块不可用时不影响主服务。
5. **长连接与短任务区分**：
   - 短任务（HTTP 请求、TCP/UDP 探测）交给 libuv 线程池；
   - 常驻任务（反向代理）运行独立 libuv 事件循环；
   - 未来常驻协议服务将独立进程化，主进程只做管理和聚合。
6. **可恢复初始化**：缺少数据库、空数据库、网页“恢复出厂设置”三种路径统一回到首次安装。

---

## 3. 里程碑

### M0 — 0.1.0 可用基线（已完成）

验收：

- 全新安装可以创建管理员、登录、创建普通用户并限制菜单。
- HTTP/1.1 + HTTPS 请求可返回状态、Header、Body、耗时和历史。
- 一个反向代理可以从 UI 创建、启动、访问、查看日志、停止、删除。
- TLS 文本解析、SOAP WSDL 解析、TCP/UDP 探测可用。
- 离线 `uvscope --self-check` 全部通过。
- `npm run build` 与 C++ Release 构建通过。

### M1 — 调试体验与响应预览（0.2.0，已完成）

目标：让“看到响应”变成“看懂并安全地预览响应”。

- 响应预览：
  - Body 增加 raw / preview 切换；
  - `text/html`、`application/xhtml+xml` 用 `iframe sandbox` + `srcdoc` 渲染，默认禁用脚本与远程资源；
  - `image/*`、`audio/*`、`video/*`、`application/pdf` 用 Blob URL 预览；
  - 二进制、截断 body、未知类型继续显示 hex/raw；
  - Blob URL 在切换响应/卸载时释放。
- 国际化：
  - `navigator.language` 自动选择 `zh-CN` / `en`；
  - 顶栏手动切换，持久化到 `localStorage`；
  - 壳层、导航、登录/初始化、设置、帮助先行，其余页面逐页补齐。
- 主题：
  - 亮色 / 暗色 / 跟随系统三档；
  - 所有页面使用 `--*` 变量，清理硬编码颜色。
- 安全说明写进 Help：预览不是安全边界，恶意 HTML 仍可能消耗资源；默认禁脚本。

验收：

- `npm --prefix web run typecheck && npm --prefix web run build`；
- 同一响应可在 raw 与 preview 间切换；HTML 中 `<script>` 不执行；
- 语言与主题刷新后保持；
- 无新增网络栈。

### M2 — 传输工作台（0.3.0，已完成）

目标：从“一次性探测”升级为“可持续观察和交互的传输会话”。

- 持久 TCP 客户端：
  - 一条连接多次发送、持续接收、主动关闭；
  - 文本/hex 发送，接收字节计数与时间线；
  - 断线原因、重连、超时与背压可观测。
- TCP 服务端：
  - 绑定/监听/停止，端口占用检测；
  - 会话列表、逐会话发送/关闭、字节统计；
  - 监听配置持久化，可选自动启动。
- UDP 服务端：
  - 绑定/收包/按来源回复；
  - 每个数据报的来源、长度、hex 与时间线。
- 统一会话事件 schema：`type/timestamp/bytes/peer/error`。
- 常驻监听继续遵守“主进程单写 SQLite”，运行时只读配置快照。

验收：

- 本地 echo 服务端上，一条 TCP 连接连续两次发送都收到响应；
- 断开后会话状态可见，重连成功；
- TCP/UDP 服务端可启动、被外部客户端访问、在 UI 中回复、停止后端口释放；
- `--self-check` 与 smoke test 覆盖最小 echo 回合。

### M2.5 — SOCKS5 客户端与服务端（0.3.0，已完成）

目标：把代理能力做成一等公民，而不是隐藏开关。

- 客户端：
  - HTTP / 持久 TCP / UDP 三类出站都可选择 SOCKS5；
  - no-auth 与 RFC 1929 用户名密码；
  - CONNECT、UDP ASSOCIATE；错误区分代理不可达、认证失败、目标拒绝、超时；
  - 代理配置错误时直接失败，不回落直连。
- 服务端：
  - 监听、no-auth / 用户名密码、会话列表；
  - CONNECT / BIND / UDP ASSOCIATE 状态；
  - 来源、目标、字节数、失败原因、连接时长；
  - 默认只监听 `127.0.0.1`，开放到局域网需要显式修改。

验收：

- 通过本机 SOCKS5 服务端访问外部 HTTP 与 TCP echo；
- `curl --socks5-hostname` 对照；
- 错误凭据、不可达目标、未授权命令有明确错误码；
- 代理失败时目标没有直连行为。

### M3 — 协议深度（0.4.x）

- HTTP/2：（进行中）显式 `protocol: "http2"` 已通过 ALPN 协商 h2，返回 stream ID、GOAWAY 与
  前 200 个接收帧摘要（类型/标志/流/载荷长度/错误码）；Linux smoke 已覆盖，Windows/macOS
  CI 的运行时验证待补。`auto` 仍走 HTTP/1.1。
  - 剩余：ALPN/GOAWAY/连接重置的失败分类细化；
  - 剩余：跨平台运行时矩阵。
- HTTP/3 / QUIC：
  - libuvcpp HTTP/3 客户端；
  - 0-RTT、连接迁移、QPACK 摘要；
  - 原始 QUIC stream / datagram 调试。
- HTTP Body：
  - gzip/br 解压与原始字节切换；
  - 大响应流式查看与下载；
  - multipart 上传、文件下载、Range 续传。
- WebSocket：
  - 可选后端代理模式，保留 opcode、mask、压缩标志；
  - Ping/Pong、Close code、重连与导出日志。
- TLS：
  - 证书链逐张解析；
  - 连接目标抓取 peer certificate；
  - 过期/域名不匹配/自签名的结构化诊断。

验收：

- HTTP/2 与 HTTP/3 在 Linux x64 和 Windows x64 CI 中可重复；
- 失败路径有可操作错误，而不是超时；
- QUIC 未通过跨平台验证前，发布构建继续关闭。

### M4 — 网关化与团队使用（0.5.x）

- 反向代理：
  - Host / Path / Header / Method 路由规则；
  - 上游组、权重、超时、重试；
  - WebSocket Upgrade 透传；
  - 请求/响应改写；
  - TLS 入站证书管理。
- 多实例治理：配置版本化、热重载、端口冲突检测、指标。
- 多用户审计日志、强制下线。
- 集合 / 环境变量 / 脚本 / HAR / cURL / OpenAPI / Postman 导入导出。
- 备份恢复、SQLite 迁移框架。
- 进程模型：SessionSupervisor / ServiceSupervisor，常驻服务子进程化。

### M5 — 1.0

- 完整测试矩阵：Linux x64/arm64、Windows x64/arm64（MinGW）、macOS arm64；
- 安装包签名、SBOM、可复现构建；
- 性能基线与内存泄漏回归；
- 完整文档、贡献指南和发布流程；
- 稳定的 IPC/事件 schema 与插件接口。

## 4. 任务拆解（按模块）

### 4.1 主服务 / 平台

| 任务 | 说明 | 优先级 |
| --- | --- | --- |
| 进程监督器 | SessionSupervisor / ServiceSupervisor，重试与回收 | P1 |
| UDS / 命名管道 IPC | 长度前缀分帧 + 背压 | P1 |
| 配置热更新 | 代理、模块、设置版本化 | P1 |
| 事件总线 | 请求日志/指标统一 schema | P1 |
| 指标聚合 | 秒级窗口、p50/p95、QPS | P2 |
| 崩溃恢复 | 子进程异常退出原因与通知 | P2 |

### 4.2 HTTP / TLS

| 任务 | 说明 | 优先级 |
| --- | --- | --- |
| 响应预览 | HTML sandbox、图片、音视频、PDF、raw/hex 回落 | P0 |
| Body 解码 | gzip/br、流式、下载 | P1 |
| HTTP/2 事件 | ALPN、stream、GOAWAY、帧摘要 | P1 |
| HTTP/3 客户端 | QPACK、0-RTT、连接迁移 | P2 |
| 上传/下载 | multipart、Range、断点续传 | P2 |
| 证书抓取 | TLS 连接 peer cert chain | P1 |

> 异步 TLS/h2 已在 Linux x64 用本地 h2 服务端复测：ALPN=h2、`send()` 返回 0、preface 正常。旧记录中的“异步 TLS 不可用”已撤回；跨平台矩阵仍待 CI。

### 4.3 网关与传输服务

| 任务 | 说明 | 优先级 |
| --- | --- | --- |
| 持久 TCP 客户端 | 多次发送、持续接收、重连、hex | P1 |
| TCP 服务端 | 监听、会话、发送、关闭、日志 | P1 |
| UDP 服务端 | 绑定、收包、按来源回复 | P1 |
| SOCKS5 客户端 | HTTP/TCP/UDP 出站，认证与错误分类 | P1 |
| SOCKS5 服务端 | CONNECT/BIND/UDP ASSOCIATE，会话日志 | P1 |
| 规则引擎 | Host/Path/Header/Method 匹配 | P1 |
| WebSocket 代理 | Upgrade 双向透传 | P1 |
| 规则编辑器 | 可视化表单 + 冲突检测 | P1 |
| TLS 证书服务 | 导入、自签、证书 ID | P2 |
| 多实例指标 | 请求、错误、延迟、字节 | P2 |

### 4.4 前端

| 任务 | 说明 | 优先级 |
| --- | --- | --- |
| 标签持久化 | 刷新后恢复标签与会话草稿 | P2 |
| 命令面板 | 全局跳转与运行 | P3 |
| 统一日志查看器 | 虚拟滚动、过滤、导出 | P2 |
| 响应式增强 | 手机/平板布局 | P2 |
| 国际化 | 浏览器语言自动 + 中/英手动切换 | P0 |
| 主题 | 亮色 / 暗色 / 跟随系统，变量化清理 | P0 |
| 响应预览 | 见 4.2，组件放在 HTTP 视图内 | P0 |
| 可访问性 | 键盘导航、ARIA、对比度 | P2 |

### 4.5 开源运营

| 任务 | 说明 | 优先级 |
| --- | --- | --- |
| CI 矩阵 | Linux x64/arm64、Windows x64/arm64、macOS | P0 |
| Release | zip / deb / rpm / Windows exe | P0 |
| 安全策略 | SECURITY.md、依赖审计 | P1 |
| 贡献指南 | 模块模板、代码风格、测试要求 | P2 |
| 文档站 | 使用手册、API 参考 | P3 |

---

## 5. 当前迭代：切片、边界与验收

按顺序推进；每个切片单独提交、单独验证。除 S0 外不跨切片合并改动。

### S0 — libuvcpp 上游补丁（已完成并退役）

- 0001 TLS SNI、0002 同步 I/O 超时、0003 `cmake --install` 完整性、0004 SOCKS5 失败阶段；
- 已提 PR：Antruly/libuvcpp #38 / #39 / #40 / #42；
- 2026-10-21：libuvcpp v1.8.2 发布后切换到发布版 SDK，删除 `patches/upstream/`，
  CMake 在 SDK < 1.8.2 时直接失败；
- 验收：`./scripts/build-libuvcpp.sh` 只克隆 `v1.8.2`，uvscope 用该 SDK 通过
  `--self-check` 与 `scripts/smoke-test.sh`。

### S1 — HTTP 响应预览（前端，已实现）

边界：只改 `web/src/views/HttpView.vue`、`web/src/styles.css`，必要时加 `web/src/lib/preview.ts`。

状态：已实现并验证（HTML/图片预览、脚本禁用、远程媒体默认关闭、默认 CSP、Blob 生命周期）。

内容：

- Body / Preview 子切换；按 `content-type` 选择渲染方式；
- HTML：`iframe sandbox` + `srcdoc`，默认 CSP `default-src 'none'`，禁脚本、禁远程资源；
- 图片/音频/视频/PDF：Blob URL；切换响应或卸载时 `revokeObjectURL`；
- `bodyTruncated` 或未知类型回落 raw/hex；
- 预览容量上限与提示。

验收：

```sh
npm --prefix web run typecheck
npm --prefix web run build
```

手工：HTML 中的 `<script>` 不执行；PNG/MP4/PDF 可预览；二进制响应仍显示 hex。

不做：后端流式下载、预览编辑、脚本执行开关。

### S2 — 国际化与主题（前端，进行中）

边界：`web/src/i18n.ts`、`web/src/stores/i18n.ts`、`web/src/stores/ui.ts`、壳层与登录/初始化页面、`styles.css`。

状态：已完成。主题三档（亮/暗/跟随系统）、中英文字典、浏览器语言检测、手动切换与持久化、
全部工作台页面翻译，以及 `npm run check:i18n` 的 key/parity 检查均已落地。

内容：

- 轻量 `t()` 字典（中/英），不引入新的运行时网络依赖；
- `navigator.language` 自动选择，手动切换持久化，`<html lang>` 同步；
- 主题三档：`light` / `dark` / `auto`，`auto` 跟随 `prefers-color-scheme`；
- 先翻译壳层、导航、登录、初始化、设置、帮助；其余页面列清单逐页补。

验收：

```sh
npm --prefix web run typecheck
npm --prefix web run build
```

手工：`zh-CN`/`en-US` 浏览器首次打开语言正确；切换后刷新保持；三档主题无闪烁。

不做：一次性翻译全部业务文案、RTL。

### S3 — 持久 TCP 客户端（后端 + 前端，已实现）

边界：新增 `src/transport_manager.*`、`/api/tcp/sessions*`、`TransportView` TCP 会话区、`docs/API.md`、Help、smoke。

状态：已实现。独立 libuv 循环线程、命令队列、连接超时扫描、写队列、事件环形缓冲、
连接/发送/接收/关闭/错误事件、前端 TCP 会话面板与 smoke 断言均已落地。

内容：

- 独立 libuv 循环线程管理多条 TCP 会话；
- connect / send / read events / close / reconnect；
- 文本与 hex；字节计数；每会话事件环形缓冲；
- 会话上限、空闲回收、错误分类。

验收：

```sh
./scripts/build.sh
./build/uvscope --self-check
bash ./scripts/smoke-test.sh ./build/uvscope
```

smoke 增加：本地 echo 服务端 + 同一连接两次发送。

不做：TLS 客户端、SOCKS5、服务端监听。

### S4 — TCP / UDP 服务端（后端 + 前端，已实现）

边界：监听配置表与迁移、`/api/listeners*`、运行时管理器、TransportView 服务端区、Help/API/smoke。

状态：已实现。TCP/UDP 监听、启停、端口冲突、TCP 接入会话、UDP 数据报时间线、
逐会话/按来源发送、配置持久化与自动启动、独立服务端页面和 smoke 断言均已落地。

内容：

- TCP/UDP 监听、启停、端口冲突、会话/数据报列表；
- 逐会话或按来源发送；字节统计；事件时间线；
- 监听配置持久化与可选自动启动。

验收：

- 外部 `nc` / `curl` 能连上并在 UI 看到会话；
- 停止后端口释放；
- smoke 覆盖一次 TCP 回显与一次 UDP 请求/响应。

不做：QUIC、TLS 终止、SOCKS5。

### S5 — SOCKS5 客户端与服务端

状态：服务端与客户端均已实现。服务端提供 `/api/socks5/servers`、独立服务端页、认证、
会话与事件日志；客户端让 HTTP / 持久 TCP / UDP 探测与会话都可选择 SOCKS5 代理，
错误按 `proxy_unreachable` / `auth_failed` / `target_rejected` / `timeout` 等分类，
配置代理后绝不回落到直连。

边界：请求字段与校验、`/api/socks5/*`、运行时管理器、UI、Help/API/smoke。

内容：

- 客户端：HTTP/TCP/UDP 可选代理，认证，CONNECT/UDP ASSOCIATE，错误分类；
- 服务端：监听、认证、会话列表、CONNECT/BIND/UDP ASSOCIATE 状态；
- 代理失败不回落直连。

验收：

- `curl --socks5-hostname` 与本机客户端都能穿过服务端；
- 错误凭据/不可达目标/未授权命令有结构化错误；
- 代理断开时目标侧无直连流量。

不做：透明代理、PAC、按域名分流。

### S6 — QUIC / HTTP/3

- 先完成 libuvcpp h3 的跨平台 CI 与真实端点回归；
- 再开放客户端、stream/datagram 调试；
- 未通过前发布构建继续关闭。

---

### 风险与回退（本迭代）

| 风险 | 处理 |
| --- | --- |
| 预览 HTML 执行恶意脚本或加载外部资源 | sandbox + CSP，默认禁脚本/远程资源；只作为“查看器” |
| 常驻会话泄漏内存/端口 | 每会话上限、空闲超时、监听停止即释放、self-check 覆盖 |
| SOCKS5 配置错误导致直连 | 代理字段存在即必须走代理，失败即失败 |
| 常驻服务阻塞主循环 | 独立循环线程；主进程只做管理和聚合 |
| 大 body 进入 JSON 导致内存放大 | 预览只处理未截断 body；后续做流式下载 |
| i18n 一次性改动过大 | 先壳层与关键页，逐页补齐，字典集中管理 |

---

## 6. 跨平台与发布

### 目标平台

- Linux x64 / arm64（deb、rpm、tar.gz）
- Windows x64（MinGW-w64，zip + Inno Setup 安装 exe）
- Windows arm64（MinGW-w64 clang + libc++，实验）
- macOS arm64（源码构建 + zip，后续补签名）

### 构建事实

- libuvcpp 1.8.2 是发布版 SDK，uvscope 不再携带本地补丁；低于 1.8.2 的 SDK 在 configure
  阶段被拒绝。1.8.2 已包含 AF_UNSPEC、TLS 证书链、SOCKS5 会话统计、总 deadline、WebSocket
  逐帧、HTTP 流式响应与 h2/h3 事件，是 0.4 协议深度切片的上游基础。
- 0.3.0 发布构建默认关闭 libuvcpp 的 QUIC、HTTP/3 选项，避免发布“能选但不可用”的协议路径；
  HTTP/2 自 1.8.2 起默认启用，`UVCPP_ENABLE_NGHTTP2=OFF` 可构建精简版。
- 本地开发可以打开这些选项做实验，但不应把它们写进稳定版发行说明。
- Windows 使用 MinGW-w64，不使用 MSVC 作为默认工具链。

### 发布产物

```text
uvscope-0.3.0-linux-x64.tar.gz
uvscope-0.3.0-linux-arm64.tar.gz
uvscope_0.3.0_amd64.deb
uvscope_0.3.0_arm64.deb
uvscope-0.3.0-1.x86_64.rpm
uvscope-0.3.0-1.aarch64.rpm
uvscope-0.3.0-mingw-x64.zip
uvscope-0.3.0-setup.exe
uvscope-0.3.0-macos-arm64.zip
```

---

## 7. AI 分步开发与 uvp-agents 配合

建议把每个里程碑拆成“可独立验证的 AI 任务”，每个任务必须带：

1. **输入产物**：例如 WSDL 样例、抓包、配置 schema。
2. **实现边界**：只改哪些文件/模块。
3. **验收命令**：例如 `uvscope --self-check`、`ctest`、`npm run build`、curl 断言。
4. **回写要求**：发现 libuvcpp/uvp 缺口时，附最小复现和补丁建议，不静默绕过。

建议的 uvp-agents 任务模板：

```text
任务：为 HTTP/3 结果增加 QPACK 摘要事件
前置：libuvcpp h3 客户端可返回响应；事件 schema v1 已冻结
改动：src/http_probe.cpp + web/src/views/HttpView.vue
验收：
  - 构建通过
  - 对 h3 测试端点返回 qpack 摘要
  - 失败时保留原始 QUIC 错误码
回写：若 libuvcpp 不暴露 QPACK 事件，记录为上游 issue，不伪造数据
```

不要在同一个 AI 任务里同时修改协议栈、前端布局和数据库 schema；每次只推进一个可验证的切片。

---

## 8. 风险清单

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| libuvcpp 异步 TLS/h2/h3 不完整 | 协议调试能力受限 | 锁定 HTTP/1.1 为稳定路径；单独立项修复并加测试 |
| SNI 缺失 | 公网 HTTPS 全部失败 | 已加补丁；CI 强制应用 |
| SQLite 多进程写 | 数据损坏/锁冲突 | 主进程单写；子进程只读快照 |
| 反向代理阻塞上游 | 单实例吞吐低 | 将代理运行时改为异步 HTTP 客户端或独立进程 |
| 本机调试服务被暴露 | 内网横向移动 | 默认 127.0.0.1、RBAC、文档警告、可选认证代理 |
| 跨平台路径/证书差异 | Windows/macOS 构建失败 | CI 三平台矩阵；证书路径可配置 |
| 前端标签内存增长 | 长会话卡顿 | 标签级状态外置、事件上限、可关闭缓存 |

---

## 9. 0.1.1 建议立即处理

1. 将 HTTP/2 / HTTP/3 按钮标注为“实验”，或在稳定版构建中从 UI 隐藏。
2. 修复 HTTP 历史记录在 `bodySize` 对 TCP/TLS 的字段映射。
3. 为反向代理增加上游超时配置与 WebSocket Upgrade 检测。
4. 增加 `scripts/smoke-test.sh`，在 CI 中真实启动服务并断言 setup/login/proxy。
5. libuvcpp 补丁已提交上游：SNI（#38）、同步 I/O 超时（#39）、`cmake --install` 完整性（#40）；跟踪 review 与三平台 CI。异步 TLS/h2 已在 Linux 复测通过，旧结论已撤回。

---
