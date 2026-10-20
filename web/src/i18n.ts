export type Locale = 'en' | 'zh-CN'

type Dictionary = Record<string, string>

import http from './i18n/http'
import ws from './i18n/ws'
import transport from './i18n/transport'
import soap from './i18n/soap'
import tls from './i18n/tls'
import proxy from './i18n/proxy'
import users from './i18n/users'
import settings from './i18n/settings'
import dashboard from './i18n/dashboard'
import about from './i18n/about'
import help from './i18n/help'
import listeners from './i18n/listeners'
import socks5 from './i18n/socks5'
import terminal from './i18n/terminal'

const fragments = [http, ws, transport, soap, tls, proxy, users, settings, dashboard, about, help, listeners, socks5, terminal]

const en: Dictionary = {
  'app.warming': 'Warming up the protocol workbench',
  'app.cannotReach': 'Cannot reach uvscope',
  'nav.overview': 'Overview',
  'nav.http': 'HTTP Studio',
  'nav.websocket': 'WebSocket',
  'nav.transport': 'TCP / UDP',
  'nav.soap': 'SOAP / WSDL',
  'nav.tls': 'TLS Inspector',
  'nav.proxy': 'Reverse Proxy',
  'nav.users': 'Users & Roles',
  'nav.settings': 'Settings',
  'nav.about': 'About',
  'nav.help': 'Help',

  'section.workspace': 'Workspace',
  'section.gateway': 'Gateway',
  'section.system': 'System',

  'shell.tagline': 'protocol workbench',
  'shell.eyebrow': 'uvscope / workspace',
  'shell.daemonOnline': 'daemon online',
  'shell.expand': 'Expand sidebar',
  'shell.collapse': 'Collapse sidebar',
  'shell.brandHome': 'Go to overview',
  'shell.openNav': 'Open navigation',
  'shell.closeTab': 'Close tab',
  'shell.rightClickHint': 'right-click a tab to close the rest',
  'shell.moduleDisabled': 'Module disabled',
  'shell.moduleDisabledBody': '{title} is disabled in Settings.',
  'shell.adminRequired': 'Administrator required',
  'shell.signedOut': 'Signed out',
  'shell.sessionExpired': 'Session expired',
  'shell.sessionExpiredBody': 'Sign in again to continue.',
  'shell.about': 'About uvscope',
  'shell.signOut': 'Sign out',
  'shell.preview': 'preview',

  'theme.dark': 'dark',
  'theme.light': 'light',
  'theme.auto': 'auto',
  'theme.switch': 'Theme: {mode} — click to change',
  'language.switch': 'Switch language',

  'auth.login.eyebrow': 'The multi-protocol workbench',
  'auth.login.title1': 'One scope for every',
  'auth.login.title2': 'web handshake.',
  'auth.login.lead':
    'Inspect HTTP, WebSocket, TCP, UDP, TLS and SOAP from a single local control plane, then put traffic to work with programmable reverse proxies.',
  'auth.login.feature1.title': 'Protocol-native',
  'auth.login.feature1.text': 'Raw frames, headers, timing and bytes.',
  'auth.login.feature2.title': 'Gateway built in',
  'auth.login.feature2.text': 'Managed HTTP reverse proxies with live logs.',
  'auth.login.feature3.title': 'Local by design',
  'auth.login.feature3.text': 'SQLite storage, RBAC and no cloud dependency.',
  'auth.login.feature4.title': 'libuvcpp engine',
  'auth.login.feature4.text': 'One event-driven C++ stack end to end.',
  'auth.login.localReady': 'local daemon ready',
  'auth.login.eyebrowCard': 'Control plane',
  'auth.login.title': 'Sign in to uvscope',
  'auth.login.leadCard': 'Use the account provisioned on this machine.',
  'auth.login.username': 'Username',
  'auth.login.password': 'Password',
  'auth.login.firstInstall': 'First install?',
  'auth.login.signingIn': 'Signing in…',
  'auth.login.enter': 'Enter workspace',
  'auth.login.required': 'Enter your username and password.',
  'auth.login.welcome': 'Welcome back',
  'auth.preview': 'preview',

  'auth.setup.chip': 'first run',
  'auth.setup.eyebrow': 'Fresh installation',
  'auth.setup.title1': 'Your workbench.',
  'auth.setup.title2': 'Your rules.',
  'auth.setup.lead':
    'uvscope stores everything locally in SQLite. Create the first administrator, then decide which protocol modules and gateway controls are available to your team.',
  'auth.setup.step1.title': 'Create administrator',
  'auth.setup.step1.text': 'Owns users, roles, settings and reset.',
  'auth.setup.step2.title': 'Choose modules',
  'auth.setup.step2.text': 'Toggle protocol surfaces after sign-in.',
  'auth.setup.step3.title': 'Start probing',
  'auth.setup.step3.text': 'Or launch your first reverse proxy.',
  'auth.setup.noTelemetry': 'no telemetry, no cloud',
  'auth.setup.eyebrowCard': 'Initialize',
  'auth.setup.title': 'Create the administrator',
  'auth.setup.leadCard': 'This account is created once. Recovery is available from Settings.',
  'auth.setup.username': 'Username',
  'auth.setup.password': 'Password',
  'auth.setup.passwordHint': 'At least 8 characters',
  'auth.setup.confirm': 'Confirm password',
  'auth.setup.confirmHint': 'Repeat the password',
  'auth.setup.initializing': 'Initializing…',
  'auth.setup.initialize': 'Initialize workbench',
  'auth.setup.storage': 'Storage',
  'auth.setup.usernameShort': 'Username must be at least 3 characters.',
  'auth.setup.passwordShort': 'Password must be at least 8 characters.',
  'auth.setup.mismatch': 'Passwords do not match.',
  'auth.setup.success': 'Workbench initialized',
  'auth.setup.successBody': 'Your administrator account is ready.',
  'auth.setup.strength.0': 'Too short',
  'auth.setup.strength.1': 'Fair',
  'auth.setup.strength.2': 'Good',
  'auth.setup.strength.3': 'Strong',
  'auth.setup.strength.4': 'Excellent',
}

const zh: Dictionary = {
  'app.warming': '正在启动协议工作台',
  'app.cannotReach': '无法连接 uvscope',
  'nav.overview': '概览',
  'nav.http': 'HTTP 工作台',
  'nav.websocket': 'WebSocket',
  'nav.transport': 'TCP / UDP',
  'nav.soap': 'SOAP / WSDL',
  'nav.tls': 'TLS 检查器',
  'nav.proxy': '反向代理',
  'nav.users': '用户与角色',
  'nav.settings': '设置',
  'nav.about': '关于',
  'nav.help': '帮助',

  'section.workspace': '工作区',
  'section.gateway': '网关',
  'section.system': '系统',

  'shell.tagline': '协议工作台',
  'shell.eyebrow': 'uvscope / 工作区',
  'shell.daemonOnline': '守护进程在线',
  'shell.expand': '展开侧栏',
  'shell.collapse': '收起侧栏',
  'shell.brandHome': '返回概览',
  'shell.openNav': '打开导航',
  'shell.closeTab': '关闭标签',
  'shell.rightClickHint': '右键标签可关闭其他标签',
  'shell.moduleDisabled': '模块已禁用',
  'shell.moduleDisabledBody': '{title} 已在设置中禁用。',
  'shell.adminRequired': '需要管理员权限',
  'shell.signedOut': '已退出登录',
  'shell.sessionExpired': '会话已过期',
  'shell.sessionExpiredBody': '请重新登录。',
  'shell.about': '关于 uvscope',
  'shell.signOut': '退出登录',
  'shell.preview': '预览',

  'theme.dark': '暗色',
  'theme.light': '亮色',
  'theme.auto': '跟随系统',
  'theme.switch': '主题：{mode}，点击切换',
  'language.switch': '切换语言',

  'auth.login.eyebrow': '多协议调试工作台',
  'auth.login.title1': '一个入口，驾驭',
  'auth.login.title2': '每一次 Web 握手。',
  'auth.login.lead':
    '在本地统一检查 HTTP、WebSocket、TCP、UDP、TLS 与 SOAP，再用可编程反向代理把流量用起来。',
  'auth.login.feature1.title': '协议原生',
  'auth.login.feature1.text': '原始帧、Header、耗时与字节。',
  'auth.login.feature2.title': '内置网关',
  'auth.login.feature2.text': '可管理的 HTTP 反向代理与实时日志。',
  'auth.login.feature3.title': '本地优先',
  'auth.login.feature3.text': 'SQLite 存储、RBAC，无云依赖。',
  'auth.login.feature4.title': 'libuvcpp 引擎',
  'auth.login.feature4.text': '端到端一套事件驱动 C++ 栈。',
  'auth.login.localReady': '本地守护进程就绪',
  'auth.login.eyebrowCard': '控制平面',
  'auth.login.title': '登录 uvscope',
  'auth.login.leadCard': '使用本机已创建的账号。',
  'auth.login.username': '用户名',
  'auth.login.password': '密码',
  'auth.login.firstInstall': '首次安装？',
  'auth.login.signingIn': '正在登录…',
  'auth.login.enter': '进入工作台',
  'auth.login.required': '请输入用户名和密码。',
  'auth.login.welcome': '欢迎回来',
  'auth.preview': '预览',

  'auth.setup.chip': '首次运行',
  'auth.setup.eyebrow': '全新安装',
  'auth.setup.title1': '你的工作台。',
  'auth.setup.title2': '你的规则。',
  'auth.setup.lead':
    'uvscope 把所有数据保存在本地 SQLite。先创建第一个管理员，再决定团队成员可以使用哪些协议模块与网关能力。',
  'auth.setup.step1.title': '创建管理员',
  'auth.setup.step1.text': '负责用户、角色、设置与重置。',
  'auth.setup.step2.title': '选择模块',
  'auth.setup.step2.text': '登录后开关协议模块。',
  'auth.setup.step3.title': '开始调试',
  'auth.setup.step3.text': '或者先启动一个反向代理。',
  'auth.setup.noTelemetry': '无遥测、无云端',
  'auth.setup.eyebrowCard': '初始化',
  'auth.setup.title': '创建管理员',
  'auth.setup.leadCard': '该账号只创建一次，之后可在设置中恢复。',
  'auth.setup.username': '用户名',
  'auth.setup.password': '密码',
  'auth.setup.passwordHint': '至少 8 个字符',
  'auth.setup.confirm': '确认密码',
  'auth.setup.confirmHint': '再次输入密码',
  'auth.setup.initializing': '正在初始化…',
  'auth.setup.initialize': '初始化工作台',
  'auth.setup.storage': '存储',
  'auth.setup.usernameShort': '用户名至少 3 个字符。',
  'auth.setup.passwordShort': '密码至少 8 个字符。',
  'auth.setup.mismatch': '两次输入的密码不一致。',
  'auth.setup.success': '工作台已初始化',
  'auth.setup.successBody': '管理员账号已就绪。',
  'auth.setup.strength.0': '太短',
  'auth.setup.strength.1': '一般',
  'auth.setup.strength.2': '良好',
  'auth.setup.strength.3': '强',
  'auth.setup.strength.4': '极强',
}

const mergedEn: Dictionary = Object.assign({}, ...fragments.map((fragment) => fragment.en), en)
const mergedZh: Dictionary = Object.assign({}, ...fragments.map((fragment) => fragment.zh), zh)

export const dictionaries: Record<Locale, Dictionary> = { en: mergedEn, 'zh-CN': mergedZh }

export function detectLocale(): Locale {
  try {
    const stored = localStorage.getItem('uvscope-locale')
    if (stored === 'en' || stored === 'zh-CN') return stored
  } catch {
    // Locale persistence is optional; detection still works without it.
  }
  if (typeof navigator !== 'undefined') {
    const candidates = navigator.languages?.length ? navigator.languages : [navigator.language]
    if (candidates.some((value) => value.toLowerCase().startsWith('zh'))) return 'zh-CN'
  }
  return 'en'
}
