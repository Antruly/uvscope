<script setup lang="ts">
import { computed, onActivated, onDeactivated, onMounted, onUnmounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import Icon from '../components/Icon.vue'
import { get, errorMessage } from '../api'
import type { Dashboard } from '../types'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'DashboardView' })

const router = useRouter()
const auth = useAuthStore()
const ui = useUiStore()
const data = ref<Dashboard | null>(null)
const loading = ref(true)
const refreshing = ref(false)
const error = ref('')
const lastUpdated = ref<Date | null>(null)
let timer: number | undefined

const errorRate = computed(() => {
  if (!data.value) return 0
  const total = data.value.metrics.httpRequests + data.value.metrics.httpErrors
  return total ? Math.round((data.value.metrics.httpErrors / total) * 1000) / 10 : 0
})

const successRate = computed(() => (100 - errorRate.value).toFixed(1))

const uptime = computed(() => {
  const seconds = data.value?.server.uptimeSeconds ?? 0
  const days = Math.floor(seconds / 86400)
  const hours = Math.floor((seconds % 86400) / 3600)
  const minutes = Math.floor((seconds % 3600) / 60)
  if (days) return `${days}d ${hours}h`
  if (hours) return `${hours}h ${minutes}m`
  return `${minutes}m ${seconds % 60}s`
})

const rss = computed(() => {
  const kb = data.value?.server.rssKb ?? 0
  return kb > 1024 ? `${(kb / 1024).toFixed(1)} MB` : `${kb} KB`
})

const recent = computed(() => data.value?.recent ?? [])
const protocolRows = computed(() => {
  const counts = data.value?.activityCounts ?? {}
  const total = Object.values(counts).reduce((a, b) => a + b, 0) || 1
  return ['http', 'tcp', 'udp', 'tls'].map((kind) => ({
    kind,
    count: counts[kind] ?? 0,
    percent: Math.round(((counts[kind] ?? 0) / total) * 100),
  }))
})

const sparkline = computed(() => {
  const values = [...recent.value].reverse().map((item) => Math.max(2, item.latencyMs || 2))
  if (!values.length) return ''
  const max = Math.max(...values, 1)
  const width = 640
  const height = 150
  const step = values.length > 1 ? width / (values.length - 1) : width
  const points = values.map((value, index) => {
    const x = index * step
    const y = height - (value / max) * (height - 22) - 8
    return `${x.toFixed(1)},${y.toFixed(1)}`
  })
  const line = `M ${points.join(' L ')}`
  const area = `${line} L ${width},${height} L 0,${height} Z`
  return { line, area, max: `${max} ms` }
})

const quickActions = computed(() =>
  [
    { label: 'New HTTP request', icon: 'bolt', to: '/http', module: 'http' },
    { label: 'Open WebSocket', icon: 'pulse', to: '/websocket', module: 'websocket' },
    { label: 'Start a reverse proxy', icon: 'route', to: '/proxy', module: 'proxy' },
    { label: 'Inspect a certificate', icon: 'shield', to: '/tls', module: 'tls' },
  ].filter((action) => !action.module || auth.moduleEnabled(action.module as never)),
)

async function load(background = false) {
  if (background) refreshing.value = true
  else loading.value = true
  error.value = ''
  try {
    data.value = await get<Dashboard>('/api/dashboard')
    lastUpdated.value = new Date()
  } catch (err) {
    error.value = errorMessage(err)
    if (!background) ui.toastError(err, 'Dashboard unavailable')
  } finally {
    loading.value = false
    refreshing.value = false
  }
}

function startPolling() {
  stopPolling()
  timer = window.setInterval(() => void load(true), 4000)
}

function stopPolling() {
  if (timer) window.clearInterval(timer)
  timer = undefined
}

function go(path: string) {
  router.push(path)
}

function kindLabel(kind: string) {
  return kind.toUpperCase()
}

function formatTime(value: string) {
  const date = new Date(value)
  const now = Date.now()
  const delta = Math.max(0, now - date.getTime())
  if (delta < 60_000) return `${Math.floor(delta / 1000)}s ago`
  if (delta < 3_600_000) return `${Math.floor(delta / 60_000)}m ago`
  if (delta < 86_400_000) return `${Math.floor(delta / 3_600_000)}h ago`
  return date.toLocaleDateString()
}

function formatBytes(bytes: number) {
  if (!bytes) return '0 B'
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`
}

onMounted(() => {
  void load()
  startPolling()
})
onActivated(() => {
  if (!data.value) void load()
  startPolling()
})
onDeactivated(stopPolling)
onUnmounted(stopPolling)
</script>

<template>
  <div class="page dashboard">
    <section class="hero">
      <div class="hero__copy">
        <div class="hero__eyebrow">
          <span class="live-pill"><i class="status-dot" /> live control plane</span>
          <span class="hero__crumb">workspace overview</span>
        </div>
        <h2>Good {{ new Date().getHours() < 12 ? 'morning' : new Date().getHours() < 18 ? 'afternoon' : 'evening' }}, {{ auth.user?.username }}.</h2>
        <p>
          One local daemon, every handshake in sight. Watch traffic, jump into a protocol module, or put a
          reverse proxy to work.
        </p>
        <div class="hero__actions">
          <button class="button button--primary" @click="go('/http')">
            <Icon name="bolt" :size="17" />Send a request
          </button>
          <button class="button" @click="go('/proxy')">
            <Icon name="route" :size="17" />Manage gateway
          </button>
        </div>
      </div>
      <div class="hero__scope" aria-hidden="true">
        <div class="scope-ring scope-ring--one" />
        <div class="scope-ring scope-ring--two" />
        <div class="scope-ring scope-ring--three" />
        <div class="scope-sweep" />
        <div class="scope-core">
          <Icon name="activity" :size="27" />
        </div>
        <span class="scope-label scope-label--a">H1</span>
        <span class="scope-label scope-label--b">WS</span>
        <span class="scope-label scope-label--c">TLS</span>
        <span class="scope-label scope-label--d">UDP</span>
      </div>
    </section>

    <div v-if="error" class="inline-alert inline-alert--danger">
      <Icon name="alert" :size="16" />{{ error }}
    </div>

    <section class="stat-grid">
      <article class="stat-card stat-card--accent">
        <div class="stat-card__top">
          <span class="stat-card__icon"><Icon name="bolt" :size="18" /></span>
          <span class="stat-card__trend">session</span>
        </div>
        <div class="stat-card__value">{{ data?.metrics.httpRequests ?? 0 }}</div>
        <div class="stat-card__label">HTTP requests</div>
        <div class="stat-card__foot"><span>{{ data?.metrics.httpErrors ?? 0 }} failed</span><span class="mono">{{ successRate }}% ok</span></div>
      </article>
      <article class="stat-card">
        <div class="stat-card__top">
          <span class="stat-card__icon"><Icon name="route" :size="18" /></span>
          <span class="stat-card__trend" :class="{ 'stat-card__trend--warn': (data?.metrics.proxyErrors ?? 0) > 0 }">gateway</span>
        </div>
        <div class="stat-card__value">{{ data?.metrics.activeProxies ?? 0 }}</div>
        <div class="stat-card__label">Active reverse proxies</div>
        <div class="stat-card__foot"><span>{{ data?.metrics.proxyRequests ?? 0 }} proxied</span><span class="mono">{{ data?.metrics.proxyErrors ?? 0 }} errors</span></div>
      </article>
      <article class="stat-card">
        <div class="stat-card__top">
          <span class="stat-card__icon"><Icon name="pulse" :size="18" /></span>
          <span class="stat-card__trend">transport</span>
        </div>
        <div class="stat-card__value">{{ (data?.metrics.tcpProbes ?? 0) + (data?.metrics.udpProbes ?? 0) }}</div>
        <div class="stat-card__label">Transport probes</div>
        <div class="stat-card__foot"><span>{{ data?.metrics.tcpProbes ?? 0 }} TCP</span><span class="mono">{{ data?.metrics.udpProbes ?? 0 }} UDP</span></div>
      </article>
      <article class="stat-card">
        <div class="stat-card__top">
          <span class="stat-card__icon"><Icon name="clock" :size="18" /></span>
          <span class="stat-card__trend">process</span>
        </div>
        <div class="stat-card__value stat-card__value--small">{{ uptime }}</div>
        <div class="stat-card__label">Daemon uptime</div>
        <div class="stat-card__foot"><span>{{ rss }} RSS</span><span class="mono">v{{ data?.server.version }}</span></div>
      </article>
    </section>

    <section class="dash-grid">
      <article class="card card--chart">
        <header class="card__head">
          <div>
            <span class="eyebrow">Telemetry</span>
            <h3>Request latency profile</h3>
          </div>
          <div class="card__tools">
            <span class="muted mono">{{ lastUpdated ? lastUpdated.toLocaleTimeString() : 'waiting' }}</span>
            <button class="icon-button" :class="{ spinning: refreshing }" title="Refresh" @click="load(false)">
              <Icon name="refresh" :size="16" />
            </button>
          </div>
        </header>
        <div class="chart">
          <div v-if="sparkline" class="chart__svg-wrap">
            <svg viewBox="0 0 640 150" preserveAspectRatio="none" class="chart__svg">
              <defs>
                <linearGradient id="latency-fill" x1="0" y1="0" x2="0" y2="1">
                  <stop offset="0" stop-color="var(--accent)" stop-opacity=".26" />
                  <stop offset="1" stop-color="var(--accent)" stop-opacity="0" />
                </linearGradient>
              </defs>
              <path :d="sparkline.area" fill="url(#latency-fill)" />
              <path :d="sparkline.line" class="chart__line" />
            </svg>
            <div class="chart__axis"><span>older</span><span class="mono">peak {{ sparkline.max }}</span><span>now</span></div>
          </div>
          <div v-else class="empty-inline">
            <Icon name="activity" :size="20" />
            <span>Latency samples appear after your first probe.</span>
          </div>
        </div>
        <div class="metric-row">
          <div><span>Success rate</span><strong>{{ successRate }}%</strong></div>
          <div><span>Failures</span><strong>{{ data?.metrics.httpErrors ?? 0 }}</strong></div>
          <div><span>TLS inspections</span><strong>{{ data?.metrics.tlsInspections ?? 0 }}</strong></div>
          <div><span>Users</span><strong>{{ data?.metrics.users ?? 0 }}</strong></div>
        </div>
      </article>

      <article class="card">
        <header class="card__head">
          <div>
            <span class="eyebrow">Mix</span>
            <h3>Protocol activity</h3>
          </div>
          <Icon name="layers" :size="18" class="muted" />
        </header>
        <div class="protocol-list">
          <div v-for="row in protocolRows" :key="row.kind" class="protocol-row">
            <div class="protocol-row__label">
              <span class="protocol-dot" :class="`protocol-dot--${row.kind}`" />
              <span>{{ kindLabel(row.kind) }}</span>
            </div>
            <div class="protocol-row__bar"><span :style="{ width: `${Math.max(2, row.percent)}%` }" /></div>
            <span class="mono protocol-row__count">{{ row.count }}</span>
          </div>
        </div>
        <div class="health-list">
          <div class="health-row">
            <span><i class="status-dot" />daemon</span><strong>healthy</strong>
          </div>
          <div class="health-row">
            <span><i class="status-dot status-dot--blue" />database</span><strong>SQLite · WAL</strong>
          </div>
          <div class="health-row">
            <span><i class="status-dot status-dot--violet" />gateway</span><strong>{{ data?.metrics.activeProxies ?? 0 }} running</strong>
          </div>
        </div>
      </article>

      <article class="card">
        <header class="card__head">
          <div>
            <span class="eyebrow">Shortcuts</span>
            <h3>Open a workbench</h3>
          </div>
          <Icon name="spark" :size="18" class="muted" />
        </header>
        <div class="quick-grid">
          <button v-for="action in quickActions" :key="action.to" class="quick-action" @click="go(action.to)">
            <span class="quick-action__icon"><Icon :name="action.icon" :size="19" /></span>
            <span>{{ action.label }}</span>
            <Icon name="arrow-right" :size="15" class="quick-action__arrow" />
          </button>
        </div>
      </article>

      <article class="card card--activity">
        <header class="card__head">
          <div>
            <span class="eyebrow">Timeline</span>
            <h3>Recent activity</h3>
          </div>
          <button class="link-button" @click="go('/settings')">History</button>
        </header>
        <div v-if="recent.length" class="activity-list">
          <div v-for="item in recent" :key="item.id" class="activity-row">
            <span class="kind-chip" :class="`kind-chip--${item.kind}`">{{ item.kind }}</span>
            <div class="activity-row__main">
              <strong>{{ item.summary }}</strong>
              <span>{{ formatTime(item.createdAt) }} · {{ item.latencyMs }} ms · {{ formatBytes(item.sizeBytes) }}</span>
            </div>
            <span class="status-code" :class="item.status === 'success' ? 'status-code--ok' : 'status-code--bad'">
              {{ item.httpStatus || (item.status === 'success' ? 'OK' : 'ERR') }}
            </span>
          </div>
        </div>
        <div v-else class="empty-inline">
          <Icon name="terminal" :size="20" />
          <span>No activity yet. Send your first request from HTTP Studio.</span>
        </div>
      </article>
    </section>
  </div>
</template>
