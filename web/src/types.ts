export interface User {
  id: number
  username: string
  role: 'admin' | 'operator' | 'viewer'
  roleDescription: string
  enabled: boolean
  createdAt: string
  lastLogin: string
}

export type ModuleMap = Record<'http' | 'websocket' | 'tcpudp' | 'soap' | 'tls' | 'proxy' | 'terminal', boolean>

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

export interface ProxyRule {
  id: string
  name: string
  enabled: boolean
  matchType: 'exact' | 'prefix' | 'regex'
  pattern: string
  method?: string
  host?: string
  headerName?: string
  headerValue?: string
  action: 'redirect' | 'rewrite'
  status: number
  destination: string
  preserveQuery: boolean
}

export interface ActiveSession {
  id: string
  userId: number
  username: string
  role: string
  createdAt: string
  expiresAt: number
  current: boolean
}

export interface AuditEntry {
  id: number
  at: string
  userId: number
  username: string
  action: string
  target: string
  detail: string
  ip: string
}

export interface ProxyUpstream {
  url: string
  weight: number
}

export interface ProxyInfo {
  id: string
  name: string
  listenHost: string
  listenPort: number
  target: string
  upstreams?: ProxyUpstream[]
  retries?: number
  connectTimeoutMs?: number
  requestTimeoutMs?: number
  workers?: number
  breakerThreshold?: number
  breakerCooldownMs?: number
  configVersion?: number
  autoStart: boolean
  tls: boolean
  certConfigured?: boolean
  certPath?: string
  keyPath?: string
  rules?: ProxyRule[]
  running: boolean
  startedAt?: string
  lastError?: string
  requests: number
  errors: number
  bytes?: number
  health?: Array<{
    url: string
    healthy: boolean
    checked: boolean
    latencyMs: number
    checkedAt: string
    error: string
    breaker?: {
      open: boolean
      failures: number
      retryAfterMs: number
      lastError: string
    }
  }>
  metrics?: {
    windowSeconds: number
    qps: number
    p50Ms: number
    p95Ms: number
    status2xx: number
    status3xx: number
    status4xx: number
    status5xx: number
  }
}

export interface HttpHeader {
  name: string
  value: string
}

export interface Socks5ProxyInfo {
  enabled?: boolean
  host?: string
  port?: number
  authenticated?: boolean
  relay?: string
  errorClass?: string
  errorCode?: number
  error?: string
}

export interface Http2Frame {
  type: string
  typeId: number
  flags: number
  streamId: number
  length: number
  errorCode: number
  lastStreamId: number
}

export interface HttpDownload {
  id: string
  state: 'running' | 'ready' | 'error' | 'cancelled'
  error?: string
  url: string
  contentType?: string
  filename?: string
  startedAt?: string
  finishedAt?: string
  bytes: number
  totalBytes: number
  statusCode?: number
  resumable?: boolean
  attempts?: number
}

export interface HttpResult {
  ok: boolean
  error?: string
  proxy?: Socks5ProxyInfo | null
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
  goaway?: { received: boolean; errorCode?: number; lastStreamId: number }
  h2?: {
    frameCount: number
    framesTruncated: boolean
    maxFrames: number
    frames: Http2Frame[]
  }
  compression?: {
    requested: boolean
    contentEncoding: string
    decoded: boolean
    wireBodySize?: number
    bodySize: number
    error?: string
  }
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
  error?: string
  code?: string
  sha256?: string
  url?: string
  host?: string
  port?: number
  certificates: CertificateEntry[]
  certificateCount: number
  chainTruncated: boolean
  chainNotice: string
  live?: {
    verified: boolean
    verifyResult: number
    verifyError: string
    alpn: string
    insecure: boolean
    handshakeError?: string
    certificateCount?: number
  }
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

export interface WsSession {
  id: string
  ownerId?: number
  owner?: string
  url: string
  state: 'connecting' | 'open' | 'closing' | 'closed' | 'error'
  error?: string
  createdAt?: string
  connectedAt?: string
  closedAt?: string
  sentBytes: number
  receivedBytes: number
  frameCount: number
  closeCode?: number
  closeReason?: string
  nextSeq: number
}

export interface WsEvent {
  seq: number
  type: string
  at?: string
  text?: string
  base64?: string
  encoding?: string
  size?: number
  truncated?: boolean
  opcode?: string
  opcodeId?: number
  fin?: boolean
  rsv1?: boolean
  masked?: boolean
  length?: number
  code?: number
  reason?: string
  error?: string
  closeCode?: number
  closeReason?: string
  status?: number
}
