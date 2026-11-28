# Windows 透明代理设计（`uvscope-transparent`）

> 状态：**设计稿**。本文只描述做法，不是已实现的功能。规则引擎已随
> `feat/address-groups-routing` 落地；本页依赖的 SOCKS5 服务端上游钩子是
> libuvcpp `Antruly/libuvcpp#121`（`route_target`）。

## 1. 目标

让 Windows 上的程序**不需要配置代理**就能被 uvscope 接管：浏览器的连接被本机拦截，
送到 uvscope 的 SOCKS5 服务端，由地址组/规则决定直连还是走上游。

非目标（本阶段不做）：

- HTTPS 中间人解密、请求/响应抓包；
- 代理链、负载均衡；
- per-进程规则（规则按目的地，落在 uvscope 服务端）；
- macOS（见 §12）。

## 2. 放在 uvscope 仓库里，不新建仓库

拦截程序是 uvscope 的一个**独立可执行 target**（`uvscope-transparent.exe`），而不是
塞进 `uvscope.exe`：

- 主程序不引入 WinDivert 依赖，普通用户不受影响；
- 安装包不包含驱动，只有这个小程序；
- 复用 uvscope 的 CI、发布流程和 supervisor/IPC 约定。

`AGENTS.md` 的"唯一网络栈"需要补一句边界：拦截层只负责把**本机连接改道到本地监听**，
它不发起目标连接；所有目标 I/O 仍然走 uvscope 的 SOCKS5（libuvcpp）。因此不构成第二套
目标网络栈。

## 3. 组件与进程模型

```
uvscope.exe (用户态, 普通权限)
   │  IPC（命名管道，长度前缀 JSON，与现有 child 通道同构）
   ▼
uvscope-transparent.exe (Windows 服务, LocalSystem)
   │  WinDivert.dll（运行时加载，用户自行安装）
   ▼
本地中继监听 127.0.0.1:<ephemeral>  ←── 被改道过来的应用连接
   │  SOCKS5 CONNECT（目标 = 原始目的地址）
   ▼
uvscope 的 SOCKS5 服务端（127.0.0.1:1080，规则在这里生效）
   │  route_target：direct / upstream
   ▼
目标
```

- 服务由安装器**注册一次**（管理员），uvscope 通过 IPC 启停；
- 服务启动参数由 uvscope 下发：SOCKS5 端口、需要排除的 PID（uvscope 自己）、日志级别；
- 中继端口由服务自己 `bind(0)` 取，注册到 WinDivert 过滤器里。

## 4. 驱动策略：不打包，运行时加载

**只用 WinDivert 2.2.2**（LGPLv3 / GPLv2 双许可）：

- 不随安装包分发 `WinDivert.dll` / `WinDivert64.sys`，也不把我们自己的 `.sys` 放进去；
- 辅助程序 `LoadLibrary` 查找顺序：exe 同目录 → `%ProgramFiles%\WinDivert` → `PATH`，
  可用 `--windivert-dir` 覆盖；
- 找不到 DLL 或首次 `WinDivertOpen` 返回权限/服务错误时，向 uvscope 报告
  `driver_missing` / `driver_service_failed`，UI 把"透明模式"置灰并给官方下载链接；
- 首次成功打开时 **WinDivert 自己安装内核服务**（需要管理员，服务已经是 LocalSystem，
  满足条件）。

不选 NetFilter SDK 的原因：商业授权，且其 demo 驱动有连接数上限（限流后需重启系统）。

## 5. TCP 数据流与重定向

实现参考 WinRoute 的 WinDivert 后端思路（其 README 与源码公开；本项目不复制其代码）。

### 5.1 过滤器

```
tcp and not impostor and (
  (outbound and not loopback) or
  (loopback and (tcp.SrcPort == relayPort or tcp.DstPort == relayPort))
)
```

- `not impostor` 排除我们自己注入的包；
- 回环只放行与中继端口相关的那几条，避免把自己和 uvscope 的流量卷进来。

### 5.2 SYN 分类

- WinDivert 会扣住 SYN，分类没有竞态；
- 用 `GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL)` 按（本地地址, 本地端口）反查 PID；
- `pid == 0`（查不到）→ 放行；
- `pid == self` 或 `pid == uvscope` → 放行（**这是与 WinRoute 最大的不同**：它只连外部
  代理，只有自己一个进程；我们连本机 uvscope，必须同时排除 uvscope 的 PID，否则
  uvscope 的出站会被再次截获形成环）；
- 其余 → 记录原始目标，进入 Active。

### 5.3 正向 NAT

把目标改成**应用自己的源地址 + 中继端口，保留应用的源端口**，并把注入地址标记改成
"入站 / 回环"，让 Windows 把它交给本地中继监听。

应用的内核 TCP 栈并不知道目标被改：它完成的是与中继的三次握手。

### 5.4 反向 NAT（必须做）

中继 → 应用的包（`Loopback && SrcPort == relayPort`）把**源**改回原始目标地址/端口。
少了这一步，应用会认为回包来自错误的对端而丢弃。

### 5.5 FlowTable

- 主键：应用的**源端口**（中继 accept 后拿 peer 端口查询）；
- 取出后用"真实目标"二次校验——中继自己的上游连接也从同一临时端口段取端口，只靠
  端口会撞；
- 2 分钟过期 + 8192 上限兜底；连接关闭时删除。

### 5.6 中继

- 监听 `AnyIPv4` / `AnyIPv6` 的临时端口（改道后的目标地址是应用源地址，可能是局域网
  IP，不能只绑回环）；
- accept 后按 FlowTable 还原目标，开一条 SOCKS5 连接到 uvscope，双向拼接；
- 连接失败按 SOCKS5 REP 映射成可读错误，写入 uvscope 的会话日志（uvscope 侧本来就会
  记录 `target`、`peer`、字节数）。

## 6. 循环排除：三重

1. 本进程 PID 放行；
2. uvscope PID 放行（IPC 下发，进程重启后更新）；
3. `not impostor` 不抓自己注入的包。

再加一层保险：如果某个进程正在监听 uvscope 的 SOCKS5 端口，判定它就是代理程序，强制
直连（WinRoute 的 `LoopGuard` 同款，用 LISTEN 表反查）。

## 7. 与规则引擎的对接

拦截器**不判断**直连还是上游，它只把连接送到 uvscope 的 SOCKS5。判断在服务端：

```
uvscope: uvcpp_socks5_server_options.route_target = [&](target, upstream) {
  auto decision = route_decide(config, groups, resolve(target));
  if (decision.action != "proxy") return false;       // 直连
  upstream->host = ...; upstream->port = ...;         // SOCKS5 上游
  return true;
};
```

这需要 libuvcpp 1.12.1（PR #121）。在 1.12.1 之前，拦截器可以先用：所有连接直连，
只验证"拦截 + 中继"这条链路本身。

## 8. UDP 与 DNS（第二阶段）

- UDP 单独一个抓包句柄（排除回环与 DNS 端口）；
- 按应用 UDP 端点建会话，走 SOCKS5 UDP ASSOCIATE；代理不支持 UDP 时回退直连；
- DNS 另开 UDP 53 句柄：嗅探应答填 IP→域名缓存，让中继能把**域名**交给 uvscope
  （规则按域名/地址匹配都更准）；"通过代理解析"开启时，出站查询假解析到
  `198.18.0.0/15`（不能用回环段，网络层无法重定向回环流量）。

第一阶段先不开启 UDP，UI 明示"UDP 暂不拦截"。

## 9. 服务化与 IPC

- 服务名 `uvscope-transparent`，显示名 "uvscope transparent capture"；
- 只接受本机命名管道，管道 ACL 限定当前用户 + SYSTEM；
- 消息与现有 child 通道同构：`start` / `stop` / `status` / `stats`；
- uvscope 的 supervisor 负责期望状态：uvscope 启动时按设置恢复，崩溃后重启服务。

## 10. 安全与权限

- 服务以 LocalSystem 运行：只做"改道 + 转发到 127.0.0.1 的 uvscope"，不落盘、不发起
  目标连接；
- 管道消息做长度上限与 JSON 校验；拒绝来自非本机管道的连接；
- 卸载流程：停止服务 → 删除服务 → 提示用户是否移除 WinDivert 服务（默认保留，因为
  其他程序可能也在用）；
- 日志只记录连接元数据（PID、应用名、目标、结果），不记录载荷。

## 11. 里程碑与验证

| 阶段 | 内容 | 验证命令 |
|---|---|---|
| M0 | 骨架 + 驱动检测 | `uvscope-transparent --check-driver`：未安装返回非 0 且说明原因；安装后打印版本 |
| M1 | TCP 透明转发（CLI 模式） | Windows 测试机：启动 uvscope + 拦截器，浏览器访问外网，uvscope 的 SOCKS5 会话出现对应 `target`；关闭拦截器后流量恢复直连 |
| M2 | 服务化 + IPC + UI 开关 | `sc query uvscope-transparent`；uvscope 页面开关后服务状态变化；服务崩溃后 supervisor 拉起 |
| M3 | 规则联动（1.12.1） | 地址组规则设为"overseas→上游"，`POST /api/routing/match` 与浏览器实际出口一致 |
| M4 | UDP / DNS | UDP echo 经代理回环；DNS 查询在 uvscope 侧可见 |

Windows 测试机已有：`molink.ssvr.top`（Win10，192.168.0.99/192.168.2.252 双网卡），
可直接跑 M1 的"第二个网段可达"用例。

## 12. 已知限制

- 需要管理员权限（服务安装一次）；安装包不含驱动，用户需自行安装 WinDivert；
- 杀软/EDR 可能对驱动加载告警，需要在文档里说明；
- macOS 不在本文范围：透明代理必须用 `NETransparentProxyProvider` 系统扩展，需要
  Developer ID 签名 + 公证 + 用户批准，无法用"装个驱动"的方式解决；
- 已存在的连接（拦截器启动前建立）不会被接管；
- 双栈：IPv4/IPv6 同时拦截，但上游代理的地址族支持取决于 uvscope 的 SOCKS5 配置。
