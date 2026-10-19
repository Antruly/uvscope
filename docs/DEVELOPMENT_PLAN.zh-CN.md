# uvscope 开发计划

> 版本：0.1.0 基线  
> 技术底座：libuvcpp 1.8.0 + Vue 3 + SQLite + libuvcpp WSDL/SOAP  
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
- libuvcpp 上游修复：TLS SNI 补丁（`patches/libuvcpp-1.8.0-sni.patch`），否则现代虚拟主机 HTTPS 握手失败。

### 1.2 0.1.0 明确不承诺的能力

- HTTP/2 与 HTTP/3 目前只保留协议偏好与事件字段，libuvcpp 异步 TLS/HTTP2/HTTP3 路径尚未完成跨平台验证，生产调试请使用 HTTP/1.1。
- 反向代理仅支持 HTTP/1.1 转发；WebSocket Upgrade、HTTP/2 入站、按路径/Host 规则匹配属于下一阶段。
- TCP/UDP 仅一次性探测，没有常驻监听与动态会话。
- 没有原始 QUIC 双向流 / datagram 调试。

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

### M0 — 0.1.0 可用基线（当前）

验收：

- 全新安装可以创建管理员、登录、创建普通用户并限制菜单。
- HTTP/1.1 + HTTPS 请求可返回状态、Header、Body、耗时和历史。
- 一个反向代理可以从 UI 创建、启动、访问、查看日志、停止、删除。
- TLS 文本解析、SOAP WSDL 解析、TCP/UDP 探测可用。
- 离线 `uvscope --self-check` 全部通过。
- `npm run build` 与 C++ Release 构建通过。

### M1 — 协议正确性与可信调试（0.2.x）

目标：把“能发请求”升级为“能解释请求”。

- HTTP/2：
  - 修复/验证 libuvcpp 异步 TLS + h2 路径；
  - 输出 ALPN、流 ID、GOAWAY、HEADERS/DATA 帧摘要；
  - 失败时明确区分 TLS、ALPN、GOAWAY、连接重置。
- HTTP/3 / QUIC：
  - 使用 libuvcpp HTTP/3 客户端；
  - 暴露 0-RTT、连接迁移、QPACK 解码摘要；
  - 原始 QUIC stream / datagram 调试。
- HTTP Body：
  - gzip/br 解压与原始字节切换；
  - 大响应流式查看与下载；
  - multipart 上传、文件下载、Range 续传。
- WebSocket：
  - 可选后端代理模式，保留帧级 opcode、mask、压缩标志；
  - Ping/Pong、Close code、重连与导出日志。
- TLS：
  - 上传证书链逐张解析；
  - 连接目标抓取 peer certificate；
  - 证书过期/域名不匹配/自签名的结构化诊断。
- 统一事件 schema：所有模块输出 `type/timestamp/payload`，前端共用解析器。

验收：

- 对 `https://httpbin.org`、本地 nginx h2、公开 h3 端点完成回归；
- HTTP/2 与 HTTP/3 在 Linux x64 和 Windows x64 CI 中可重复；
- 失败路径有可操作错误，而不是超时。

### M2 — 网关化（0.3.x）

目标：从“调试客户端”升级为“可编程本地网关”。

- 反向代理：
  - 路由规则（Host / Path / Header / Method）可视化编辑；
  - 上游组、权重、超时、重试；
  - WebSocket Upgrade 透传；
  - 请求/响应改写（Header、前缀剥离）；
  - TLS 入站证书管理（导入/自签/选择）。
- 多实例治理：
  - 配置版本化、热重载、端口冲突检测；
  - 实例级指标（QPS、p50/p95、错误率、流量）；
  - 访问日志落 SQLite/文件，支持导出。
- 常驻 TCP/UDP 服务端：
  - 监听、会话列表、实时发送；
  - 十六进制视图与时间线。
- 进程模型升级：
  - SessionSupervisor / ServiceSupervisor；
  - 常驻代理和协议服务支持独立子进程（stdio / UDS），崩溃回收；
  - 主进程提供统一进程拓扑视图。

验收：

- 同一时间运行 4 个代理实例 + 1 个 TCP 服务端 + 多个调试标签；
- 停止/重启主服务时，子服务行为可预期（自动退出或回收）；
- 代理规则变更不重启监听端口。

### M3 — 产品化与团队使用（0.4.x）

- 多用户审计日志、操作记录、会话管理（强制下线）。
- 请求集合 / 环境变量 / 变量替换 / 脚本前置后置（类似 Postman Collections）。
- 导入导出：HAR、cURL、OpenAPI、WSDL、Postman Collection。
- 定时任务与健康检查。
- 主题、快捷键、命令面板、国际化（中/英）。
- 升级检查与强制最低版本策略。
- 数据备份/恢复、SQLite 迁移框架、模块级 reset。

验收：

- 团队可以在同一台调试机上共享 uvscope，角色边界清晰；
- 配置与历史可备份、导出、恢复；
- 升级不丢数据。

### M4 — 1.0

- 完整测试矩阵：Linux x64/arm64、Windows x64/arm64（MinGW）、macOS arm64；
- 安装包签名、SBOM、可复现构建；
- 性能基线与内存泄漏回归；
- 完整文档、贡献指南和发布流程；
- 稳定的 IPC/事件 schema 与插件接口。

---

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
| 异步 TLS 修复 | libuvcpp memory BIO 写路径 | P0 |
| HTTP/2 事件 | ALPN、stream、GOAWAY、帧摘要 | P1 |
| HTTP/3 客户端 | QPACK、0-RTT、连接迁移 | P1 |
| Body 解码 | gzip/br、流式、下载 | P1 |
| 上传/下载 | multipart、Range、断点续传 | P2 |
| 证书抓取 | TLS 连接 peer cert chain | P1 |

### 4.3 网关

| 任务 | 说明 | 优先级 |
| --- | --- | --- |
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
| 国际化 | 中/英 | P3 |
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

## 5. 跨平台与发布

### 目标平台

- Linux x64 / arm64（deb、rpm、tar.gz）
- Windows x64（MinGW-w64，zip + Inno Setup 安装 exe）
- Windows arm64（MinGW-w64 clang + libc++，实验）
- macOS arm64（源码构建 + zip，后续补签名）

### 构建事实

- libuvcpp 1.8.0 的上游 TLS 客户端缺少 SNI。uvscope 携带补丁，CI 构建时应用到 libuvcpp 源码。
- 0.1.0 发布构建默认关闭 libuvcpp 的 HTTP/2、QUIC、HTTP/3 选项，避免发布“能选但不可用”的协议路径。
- 本地开发可以打开这些选项做实验，但不应把它们写进稳定版发行说明。
- Windows 使用 MinGW-w64，不使用 MSVC 作为默认工具链。

### 发布产物

```text
uvscope-0.1.0-linux-x64.tar.gz
uvscope-0.1.0-linux-arm64.tar.gz
uvscope_0.1.0_amd64.deb
uvscope_0.1.0_arm64.deb
uvscope-0.1.0-1.x86_64.rpm
uvscope-0.1.0-1.aarch64.rpm
uvscope-0.1.0-mingw-x64.zip
uvscope-0.1.0-setup.exe
uvscope-0.1.0-macos-arm64.zip
```

---

## 6. AI 分步开发与 uvp-agents 配合

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

## 7. 风险清单

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

## 8. 0.1.1 建议立即处理

1. 将 HTTP/2 / HTTP/3 按钮标注为“实验”，或在稳定版构建中从 UI 隐藏。
2. 修复 HTTP 历史记录在 `bodySize` 对 TCP/TLS 的字段映射。
3. 为反向代理增加上游超时配置与 WebSocket Upgrade 检测。
4. 增加 `scripts/smoke-test.sh`，在 CI 中真实启动服务并断言 setup/login/proxy。
5. 将 libuvcpp 补丁提交上游（SNI）；跟踪异步 TLS 写路径问题。
