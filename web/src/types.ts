export interface User {
  id: number
  username: string
  role: 'admin' | 'operator' | 'viewer'
  roleDescription: string
  enabled: boolean
  createdAt: string
  lastLogin: string
}

export type ModuleMap = Record<'http' | 'websocket' | 'tcpudp' | 'soap' | 'tls' | 'proxy', boolean>

export interface SetupStatus {
  ok: boolean
  initialized: boolean
  setupRequired: boolean
  version: string
  libuvcpp: string
  dataDir: string
}

export interface AuthResponse {
  ok: boolean
  user: User
  modules: ModuleMap
  permissions?: Record<string, boolean>
}

export interface ActivityItem {
  id: number
  kind: string
  summary: string
  status: string
  httpStatus: number
  latencyMs: number
  sizeBytes: number
  createdAt: string
}

export interface Dashboard {
  ok: boolean
  server: {
    version: string
    libuvcpp: string
    libuv: string
    platform: string
    startedAt: string
    uptimeSeconds: number
    host: string
    port: number
    tls: boolean
    dataDir: string
    webRoot: string
    rssKb: number
  }
  metrics: {
    httpRequests: number
    httpErrors: number
    tcpProbes: number
    udpProbes: number
    tlsInspections: number
    proxyRequests: number
    proxyErrors: number
    activeProxies: number
    users: number
    historyEntries: number
  }
  activityCounts: Record<string, number>
  modules: ModuleMap
  proxies: ProxyInfo[]
  recent: ActivityItem[]
}

export interface ProxyInfo {
  id: string
  name: string
  listenHost: string
  listenPort: number
  target: string
  autoStart: boolean
  tls: boolean
  certConfigured?: boolean
  certPath?: string
  keyPath?: string
  running: boolean
  startedAt?: string
  lastError?: string
  requests: number
  errors: number
  bytes?: number
}

export interface HttpHeader {
  name: string
  value: string
}

export interface ProxyInfo {
  enabled?: boolean
  host?: string
  port?: number
  authenticated?: boolean
  relay?: string
  errorClass?: string
  errorCode?: number
  error?: string
}

export interface HttpResult {
  ok: boolean
  error?: string
  proxy?: ProxyInfo | null
  request?: {
    method: string
    url: string
    protocolPreference?: string
    host?: string
    port?: number
    path?: string
  }
  status?: number
  statusText?: string
  httpVersion?: string
  protocol?: string
  alpn?: string
  headers?: HttpHeader[]
  bodySize?: number
  bodyTruncated?: boolean
  bodyEncoding?: 'utf8' | 'base64'
  bodyText?: string
  bodyBase64?: string
  timing?: { connectMs?: number; totalMs?: number }
  streamId?: number
  retryable?: boolean
  goaway?: { received: boolean; errorCode: number; lastStreamId: number }
  startedAt?: string
  finishedAt?: string
}

export interface HistoryEntry {
  id: number
  kind: string
  summary: string
  status: string
  httpStatus: number
  latencyMs: number
  sizeBytes: number
  detail: Record<string, unknown>
  createdAt: string
}

export interface ProxyLog {
  seq: number
  time: string
  method: string
  path: string
  status: number
  latencyMs: number
  bytes?: number
  error?: string
  message?: string
}

export interface CertificateEntry {
  index: number
  leaf: boolean
  version: number
  serial: string
  subject: string
  issuer: string
  notBefore: string
  notAfter: string
  notBeforeEpoch: number
  notAfterEpoch: number
  daysRemaining: number
  expired: boolean
  notYetValid: boolean
  selfSigned: boolean
  fingerprintSha256: string
  signatureAlgorithm: string
  publicKey: { algorithm: string; bits: number }
  san: Array<{ type: string; value: string }>
  keyUsage: string[]
  extendedKeyUsage: string[]
  derSha256: string
}

export interface CertificateInfo extends CertificateEntry {
  ok: boolean
  sha256?: string
  certificates: CertificateEntry[]
  certificateCount: number
  chainTruncated: boolean
  chainNotice: string
}

export interface UpdateInfo {
  ok: boolean
  error?: string
  currentVersion: string
  latestVersion: string
  updateAvailable: boolean
  checkedAt: string
  releaseName?: string
  releaseNotes?: string
  publishedAt?: string
  htmlUrl?: string
  starCount?: number
  cached?: boolean
}
