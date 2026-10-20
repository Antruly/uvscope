const en: Record<string, string> = {
  'help.eyebrow': 'Help',
  'help.title': 'Start with one handshake.',
  'help.lead':
    'Short guides for every workbench, plus the operational details that make local debugging predictable.',
  'help.openHttp': 'Open HTTP Studio',

  'help.quickStart': 'Quick start',
  'help.threeMoves': 'Three moves to a useful trace.',
  'help.step1.title': 'Pick a module',
  'help.step1.text': 'Use the left rail. Each module opens a cached tab and keeps its state.',
  'help.step2.title': 'Send something small',
  'help.step2.text':
    'A GET, a WebSocket ping or a TCP HEAD request is enough to validate connectivity.',
  'help.step3.title': 'Read the wire',
  'help.step3.text':
    'Headers, timing and byte counts are all local. Use history to replay a shape.',
  'help.loopRunning': 'loop running · Ctrl+C to stop',

  'help.modules.http.title': 'HTTP Studio',
  'help.modules.http.text':
    'Send any method, tune headers and body, and read status, timing and raw bytes. Ctrl+Enter sends from anywhere in the request pane.',
  'help.modules.websocket.title': 'WebSocket console',
  'help.modules.websocket.text':
    'Connects from the browser, so the URL must be reachable by this machine. Text and hex frames are captured in the tab while it is cached.',
  'help.modules.listeners.title': 'TCP / UDP servers',
  'help.modules.listeners.text':
    'Bind a local TCP or UDP port, inspect accepted sessions or datagrams, and send replies from the workbench. Listener configs persist and can auto-start with the daemon.',
  'help.modules.terminal.title': 'Virtual terminal',
  'help.modules.terminal.text':
    'A real server-side PTY per user. Sessions keep running when you refresh or switch menus; use Ctrl+Shift+C/V for copy and paste. Admins can list and close every user terminal. The module is off by default because it grants shell access.',
  'help.modules.socks5.title': 'SOCKS5 server',
  'help.modules.socks5.text':
    'Run a local SOCKS5 proxy with optional username/password authentication. Each server has its own loop; sessions show the command, target, peer and rejection reason. Byte counters are not available from the runtime, so none are shown.',
  'help.modules.tcpudp.title': 'TCP / UDP bench',
  'help.modules.tcpudp.text':
    'One-shot probes through libuvcpp, plus persistent TCP sessions with repeated sends and a per-session event log. Hex mode accepts spaced, colon-separated or plain hex payloads.',
  'help.modules.preview.title': 'Response preview',
  'help.modules.preview.text':
    'HTML renders in a sandbox with scripts disabled by default. Remote CSS/images are opt-in; “Run scripts (unsafe)” renders the page on an opaque origin without uvscope cookies. Images, audio, video and PDF load from a local Blob.',
  'help.modules.soap.title': 'SOAP / WSDL',
  'help.modules.soap.text':
    'Fetch a WSDL through the daemon, pick an operation, edit the generated SOAP 1.1 envelope, then invoke it like any HTTP request.',
  'help.modules.tls.title': 'TLS Inspector',
  'help.modules.tls.text':
    'Decodes a PEM bundle certificate by certificate, or one DER certificate. You can also fetch an https:// URL directly to capture the live peer chain, ALPN and verification result; invalid certificates are still shown with the OpenSSL reason.',
  'help.modules.proxy.title': 'Reverse proxy',
  'help.modules.proxy.text':
    'Each listener owns a host:port and forwards HTTP/1.1 requests to one upstream. Ordered exact/prefix/regex rules can redirect or rewrite paths before forwarding; logs stream into the gateway panel every few seconds.',

  'help.keyboard': 'Keyboard',
  'help.shortcuts': 'Shortcuts',
  'help.shortcut.send': 'Send the current HTTP request or WebSocket message',
  'help.shortcut.tabs': 'Close every other closable tab',
  'help.shortcut.history': 'Restore method, URL and protocol preference',
  'help.shortcut.address': 'Focus the browser address bar (browser shortcut)',

  'help.operations': 'Operations',
  'help.localBehaviour': 'Local behaviour',
  'help.note.storage': 'Storage',
  'help.note.storageText': 'SQLite in the data directory. Delete it while stopped to reset.',
  'help.note.sessions': 'Sessions',
  'help.note.sessionsText': 'HttpOnly cookie, seven-day expiry, server-side lookup.',
  'help.note.network': 'Network',
  'help.note.networkText': 'All requests originate from the uvscope daemon, not the browser.',
  'help.note.preview': 'Preview',
  'help.note.previewText':
    'Sandboxed HTML never runs scripts. Remote images and media load only after you tick the option.',
  'help.note.language': 'Language & theme',
  'help.note.languageText':
    'Both persist locally. Language follows the browser on first visit; theme can follow the system.',
  'help.note.exposure': 'Exposure',
  'help.note.exposureText': 'Bind to 127.0.0.1 unless you have a trusted private network.',

  'help.troubleshooting': 'Troubleshooting',
  'help.whenStuck': 'When something looks stuck',
  'help.tls.title': 'HTTPS fails to connect',
  'help.tls.textBefore':
    'Use a CA bundle that matches the daemon and verify the system clock. For private lab services, enable ',
  'help.tls.insecure': 'Allow insecure TLS',
  'help.tls.textAfter': ' only while testing.',
  'help.http2.title': 'HTTP/2 or HTTP/3 errors',
  'help.http2.text':
    'Protocol negotiation is experimental in 0.3.0. Use HTTP/1.1 for production debugging; the daemon reports the negotiated ALPN in the timing tab.',
  'help.proxy.title': 'A proxy port will not bind',
  'help.proxy.text':
    'Another process owns the port, or the listener is already running. Stop the old proxy, choose another port, and check the listener host.',
  'help.ws.title': 'WebSocket blocks on ws://',
  'help.ws.text':
    'Browsers block insecure WebSockets from HTTPS pages. Use wss:// or open the workbench over plain HTTP on localhost.',

  'help.cli': 'CLI',
  'help.flags': 'Useful flags',
  'help.cli.host': 'Listen address, default 127.0.0.1',
  'help.cli.port': 'Workbench port, default 8787',
  'help.cli.dataDir': 'SQLite and runtime data',
  'help.cli.webRoot': 'Built frontend directory',
  'help.cli.tls': 'Serve the UI over HTTPS',
  'help.cli.selfCheck': 'Offline smoke test',
  'help.cli.version': 'Print version and exit',
}

const zh: Record<string, string> = {
  'help.eyebrow': '帮助',
  'help.title': '从一次握手开始。',
  'help.lead': '每个工作台的简明指南，以及让本地调试更可预期的运维细节。',
  'help.openHttp': '打开 HTTP 工作台',

  'help.quickStart': '快速开始',
  'help.threeMoves': '三步得到有用的追踪。',
  'help.step1.title': '选择模块',
  'help.step1.text': '使用左侧栏。每个模块会打开一个缓存标签并保留状态。',
  'help.step2.title': '发送一个小请求',
  'help.step2.text': '一个 GET、一条 WebSocket ping 或一个 TCP HEAD 请求就足以验证连通性。',
  'help.step3.title': '读取链路上的数据',
  'help.step3.text': 'Header、耗时与字节数全部在本地。可用历史记录重放请求形态。',
  'help.loopRunning': '事件循环运行中 · Ctrl+C 停止',

  'help.modules.http.title': 'HTTP 工作台',
  'help.modules.http.text':
    '发送任意方法，调整 Header 与请求体，查看状态、耗时与原始字节。在请求面板任意位置按 Ctrl+Enter 即可发送。',
  'help.modules.websocket.title': 'WebSocket 控制台',
  'help.modules.websocket.text':
    '由浏览器发起连接，因此 URL 必须能从本机访问。标签页缓存期间会捕获文本与十六进制帧。',
  'help.modules.listeners.title': 'TCP / UDP 服务端',
  'help.modules.listeners.text':
    '绑定本地 TCP 或 UDP 端口，检查接入会话或数据报，并在工作台内直接回复。监听配置会持久化，并随守护进程自动启动。',
  'help.modules.terminal.title': '虚拟终端',
  'help.modules.terminal.text':
    '每个用户一个真实的服务端 PTY。刷新或切换菜单不会终止会话；复制粘贴使用 Ctrl+Shift+C/V。管理员可列出并关闭所有用户的终端。该模块默认关闭，因为开启等于授予 Shell 权限。',
  'help.modules.socks5.title': 'SOCKS5 服务端',
  'help.modules.socks5.text':
    '运行本地 SOCKS5 代理，可选用户名密码认证。每个服务端独占一条事件循环；会话显示命令、目标、来源与拒绝原因。运行时没有字节计数，因此界面不显示。',
  'help.modules.tcpudp.title': 'TCP / UDP 调试台',
  'help.modules.tcpudp.text':
    '通过 libuvcpp 执行一次性探测，并支持可连续发送、带逐会话事件日志的持久 TCP 会话。十六进制模式支持带空格、冒号分隔或纯十六进制负载。',
  'help.modules.preview.title': '响应预览',
  'help.modules.preview.text':
    'HTML 默认在禁用脚本的沙箱中渲染。远程 CSS/图片需手动开启；“运行脚本（不安全）”会在没有 uvscope Cookie 的 opaque origin 中完整渲染。图片、音频、视频与 PDF 从本地 Blob 加载。',
  'help.modules.soap.title': 'SOAP / WSDL',
  'help.modules.soap.text':
    '通过守护进程获取 WSDL，选择操作，编辑生成的 SOAP 1.1 信封，然后像普通 HTTP 请求一样调用。',
  'help.modules.tls.title': 'TLS 检查器',
  'help.modules.tls.text':
    '逐张解析 PEM 证书包，或解析单张 DER 证书。也可以直接抓取 https:// 地址，获取实时证书链、ALPN 与校验结果；无效证书仍会显示并附带 OpenSSL 原因。',
  'help.modules.proxy.title': '反向代理',
  'help.modules.proxy.text':
    '每个监听器占用一个 host:port，并将 HTTP/1.1 请求转发到一个上游。按顺序执行的精确/前缀/正则规则可以在转发前跳转或改写路径；日志每隔几秒流入网关面板。',

  'help.keyboard': '键盘',
  'help.shortcuts': '快捷键',
  'help.shortcut.send': '发送当前 HTTP 请求或 WebSocket 消息',
  'help.shortcut.tabs': '关闭其他所有可关闭的标签',
  'help.shortcut.history': '恢复方法、URL 与协议偏好',
  'help.shortcut.address': '聚焦浏览器地址栏（浏览器快捷键）',

  'help.operations': '运维',
  'help.localBehaviour': '本地行为',
  'help.note.storage': '存储',
  'help.note.storageText': '数据目录中的 SQLite。停止服务后删除即可重置。',
  'help.note.sessions': '会话',
  'help.note.sessionsText': 'HttpOnly Cookie，七天过期，服务端查询。',
  'help.note.network': '网络',
  'help.note.networkText': '所有请求均由 uvscope 守护进程发起，而非浏览器。',
  'help.note.preview': '预览',
  'help.note.previewText': '沙箱 HTML 从不执行脚本。远程图片与媒体仅在勾选选项后加载。',
  'help.note.language': '语言与主题',
  'help.note.languageText': '二者都保存在本地。语言在首次访问时跟随浏览器；主题可跟随系统。',
  'help.note.exposure': '暴露面',
  'help.note.exposureText': '除非处于可信私有网络，否则绑定到 127.0.0.1。',

  'help.troubleshooting': '故障排查',
  'help.whenStuck': '当一切看起来卡住时',
  'help.tls.title': 'HTTPS 连接失败',
  'help.tls.textBefore': '使用与守护进程匹配的 CA 证书包，并核对系统时间。私有实验环境仅在测试时启用',
  'help.tls.insecure': '“允许不安全的 TLS”',
  'help.tls.textAfter': '。',
  'help.http2.title': 'HTTP/2 或 HTTP/3 错误',
  'help.http2.text':
    '协议协商在 0.3.0 中仍属实验性。生产调试请使用 HTTP/1.1；守护进程会在耗时标签中报告协商出的 ALPN。',
  'help.proxy.title': '代理端口无法绑定',
  'help.proxy.text':
    '端口被其他进程占用，或监听器已在运行。停止旧代理，换一个端口，并检查监听地址。',
  'help.ws.title': 'WebSocket 在 ws:// 下被阻止',
  'help.ws.text':
    '浏览器会阻止来自 HTTPS 页面的不安全 WebSocket。请使用 wss://，或在 localhost 上通过普通 HTTP 打开工作台。',

  'help.cli': 'CLI',
  'help.flags': '常用参数',
  'help.cli.host': '监听地址，默认 127.0.0.1',
  'help.cli.port': '工作台端口，默认 8787',
  'help.cli.dataDir': 'SQLite 与运行时数据',
  'help.cli.webRoot': '构建后的前端目录',
  'help.cli.tls': '通过 HTTPS 提供界面',
  'help.cli.selfCheck': '离线冒烟测试',
  'help.cli.version': '打印版本并退出',
}

export default { en, zh }
