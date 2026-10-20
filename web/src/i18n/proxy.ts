const en: Record<string, string> = {
  'proxy.eyebrow': 'Gateway',
  'proxy.title': 'Put traffic on a leash.',
  'proxy.lead':
    'Launch HTTP reverse proxies on this machine, watch requests, and stop them without touching a config file.',
  'proxy.new': 'New proxy',

  'proxy.stats.active': 'active listeners',
  'proxy.stats.requests': 'proxied requests',
  'proxy.stats.errors': 'upstream errors',
  'proxy.stats.routes': 'configured routes',

  'proxy.readOnly': 'Your role can view proxy state but cannot start or change listeners.',
  'proxy.loading': 'Loading gateway…',

  'proxy.empty.title': 'No reverse proxies yet',
  'proxy.empty.body':
    'Point a local port at an upstream service. uvscope handles the listener, request forwarding and live trace.',
  'proxy.empty.action': 'Create the first proxy',

  'proxy.card.edit': 'Edit proxy',
  'proxy.card.delete': 'Delete proxy',
  'proxy.card.auto': 'auto',
  'proxy.card.listen': 'Listen',
  'proxy.card.upstream': 'Upstream',
  'proxy.card.requests': 'requests',
  'proxy.card.errors': 'errors',
  'proxy.card.bytes': 'bytes',
  'proxy.card.stop': 'Stop',
  'proxy.card.start': 'Start',
  'proxy.card.logs': 'Logs',

  'proxy.status.running': 'running',
  'proxy.status.error': 'error',
  'proxy.status.stopped': 'stopped',

  'proxy.logs.eyebrow': 'Live trace',
  'proxy.logs.close': 'Close logs',
  'proxy.logs.empty': 'No proxy events yet.',
  'proxy.logs.errorCode': 'ERR',

  'proxy.editor.eyebrow': 'Reverse proxy',
  'proxy.editor.editTitle': 'Edit listener',
  'proxy.editor.newTitle': 'New listener',
  'proxy.editor.name': 'Name',
  'proxy.editor.namePlaceholder': 'Staging API',
  'proxy.editor.listenHost': 'Listen host',
  'proxy.editor.listenPort': 'Listen port',
  'proxy.editor.target': 'Upstream target',
  'proxy.editor.autoStart': 'Start automatically',
  'proxy.editor.tls': 'HTTPS listener',
  'proxy.editor.certPath': 'Certificate path',
  'proxy.editor.keyPath': 'Private key path',
  'proxy.editor.hint':
    'HTTP/1.1 forwarding only in 0.3.0. WebSocket upgrade and per-path rules are on the roadmap.',
  'proxy.editor.cancel': 'Cancel',
  'proxy.editor.save': 'Save & start',

  'proxy.error.load': 'Could not load proxies',
  'proxy.error.nameRequired': 'Give this proxy a name.',
  'proxy.error.portRequired': 'A listen port is required.',
  'proxy.error.targetScheme': 'Target must start with http:// or https://.',
  'proxy.error.tlsPaths': 'TLS listener requires both a certificate and private key path.',
  'proxy.error.start': 'Could not start proxy',
  'proxy.error.stop': 'Could not stop proxy',

  'proxy.notify.created': 'Proxy created',
  'proxy.notify.updated': 'Proxy updated',
  'proxy.notify.started': 'Proxy started',
  'proxy.notify.stopped': 'Proxy stopped',
  'proxy.notify.deleted': 'Proxy deleted',

  'proxy.confirm.delete': 'Delete reverse proxy “{name}”? Its listener will be stopped.',
}

const zh: Record<string, string> = {
  'proxy.eyebrow': '网关',
  'proxy.title': '把流量牢牢管住。',
  'proxy.lead': '在本机启动 HTTP 反向代理，观察请求流转，无需改动配置文件即可随时停止。',
  'proxy.new': '新建代理',

  'proxy.stats.active': '运行中的监听器',
  'proxy.stats.requests': '代理请求',
  'proxy.stats.errors': '上游错误',
  'proxy.stats.routes': '已配置路由',

  'proxy.readOnly': '当前角色可查看代理状态，但不能启动或修改监听器。',
  'proxy.loading': '正在加载网关…',

  'proxy.empty.title': '还没有反向代理',
  'proxy.empty.body': '把本地端口指向一个上游服务，uvscope 负责监听、转发与实时追踪。',
  'proxy.empty.action': '创建第一个代理',

  'proxy.card.edit': '编辑代理',
  'proxy.card.delete': '删除代理',
  'proxy.card.auto': '自动',
  'proxy.card.listen': '监听',
  'proxy.card.upstream': '上游',
  'proxy.card.requests': '请求',
  'proxy.card.errors': '错误',
  'proxy.card.bytes': '字节',
  'proxy.card.stop': '停止',
  'proxy.card.start': '启动',
  'proxy.card.logs': '日志',

  'proxy.status.running': '运行中',
  'proxy.status.error': '异常',
  'proxy.status.stopped': '已停止',

  'proxy.logs.eyebrow': '实时追踪',
  'proxy.logs.close': '关闭日志',
  'proxy.logs.empty': '暂无代理事件。',
  'proxy.logs.errorCode': '错误',

  'proxy.editor.eyebrow': '反向代理',
  'proxy.editor.editTitle': '编辑监听器',
  'proxy.editor.newTitle': '新建监听器',
  'proxy.editor.name': '名称',
  'proxy.editor.namePlaceholder': '预发布 API',
  'proxy.editor.listenHost': '监听地址',
  'proxy.editor.listenPort': '监听端口',
  'proxy.editor.target': '上游目标',
  'proxy.editor.autoStart': '自动启动',
  'proxy.editor.tls': 'HTTPS 监听',
  'proxy.editor.certPath': '证书路径',
  'proxy.editor.keyPath': '私钥路径',
  'proxy.editor.hint': '0.3.0 仅支持 HTTP/1.1 转发。WebSocket 升级与按路径路由已在规划中。',
  'proxy.editor.cancel': '取消',
  'proxy.editor.save': '保存并启动',

  'proxy.error.load': '无法加载代理列表',
  'proxy.error.nameRequired': '请为该代理填写名称。',
  'proxy.error.portRequired': '必须填写监听端口。',
  'proxy.error.targetScheme': '目标地址必须以 http:// 或 https:// 开头。',
  'proxy.error.tlsPaths': 'TLS 监听器必须同时提供证书与私钥路径。',
  'proxy.error.start': '无法启动代理',
  'proxy.error.stop': '无法停止代理',

  'proxy.notify.created': '代理已创建',
  'proxy.notify.updated': '代理已更新',
  'proxy.notify.started': '代理已启动',
  'proxy.notify.stopped': '代理已停止',
  'proxy.notify.deleted': '代理已删除',

  'proxy.confirm.delete': '删除反向代理“{name}”？其监听器将被停止。',
}

export default { en, zh }
