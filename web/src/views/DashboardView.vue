<script setup lang="ts">
import { computed, onActivated, onDeactivated, onMounted, onUnmounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import Icon from '../components/Icon.vue'
import { get, errorMessage } from '../api'
import type { Dashboard } from '../types'
import { useAuthStore } from '../stores/auth'
import { useI18nStore } from '../stores/i18n'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'DashboardView' })

const router = useRouter()
const auth = useAuthStore()
const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
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
  if (days) return t('dashboard.duration.dayHours', { days, hours })
  if (hours) return t('dashboard.duration.hoursMinutes', { hours, minutes })
  return t('dashboard.duration.minutesSeconds', { minutes, seconds: seconds % 60 })
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
    { label: t('dashboard.action.http'), icon: 'bolt', to: '/http', module: 'http' },
    { label: t('dashboard.action.websocket'), icon: 'pulse', to: '/websocket', module: 'websocket' },
    { label: t('dashboard.action.proxy'), icon: 'route', to: '/proxy', module: 'proxy' },
    { label: t('dashboard.action.tls'), icon: 'shield', to: '/tls', module: 'tls' },
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
    if (!background) ui.toastError(err, t('dashboard.unavailable'))
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
  if (delta < 60_000) return t('dashboard.secondsAgo', { count: Math.floor(delta / 1000) })
  if (delta < 3_600_000) return t('dashboard.minutesAgo', { count: Math.floor(delta / 60_000) })
  if (delta < 86_400_000) return t('dashboard.hoursAgo', { count: Math.floor(delta / 3_600_000) })
  return date.toLocaleDateString()
}

function greeting() {
  const hour = new Date().getHours()
  const key = hour < 12 ? 'dashboard.greeting.morning' : hour < 18 ? 'dashboard.greeting.afternoon' : 'dashboard.greeting.evening'
  return t(key, { name: auth.user?.username ?? '' })
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
          <span class="live-pill"><i class="status-dot" /> {{ t('dashboard.live') }}</span>
          <span class="hero__crumb">{{ t('dashboard.crumb') }}</span>
        </div>
        <h2>{{ greeting() }}</h2>
        <p>{{ t('dashboard.lead') }}</p>
        <div class="hero__actions">
          <button class="button button--primary" @click="go('/http')">
            <Icon name="bolt" :size="17" />{{ t('dashboard.sendRequest') }}
          </button>
          <button class="button" @click="go('/proxy')">
            <Icon name="route" :size="17" />{{ t('dashboard.manageGateway') }}
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
          <span class="stat-card__trend">{{ t('dashboard.session') }}</span>
        </div>
        <div class="stat-card__value">{{ data?.metrics.httpRequests ?? 0 }}</div>
        <div class="stat-card__label">{{ t('dashboard.httpRequests') }}</div>
        <div class="stat-card__foot"><span>{{ t('dashboard.failed', { count: data?.metrics.httpErrors ?? 0 }) }}</span><span class="mono">{{ t('dashboard.okRate', { rate: successRate }) }}</span></div>
      </article>
      <article class="stat-card">
        <div class="stat-card__top">
          <span class="stat-card__icon"><Icon name="route" :size="18" /></span>
          <span class="stat-card__trend" :class="{ 'stat-card__trend--warn': (data?.metrics.proxyErrors ?? 0) > 0 }">{{ t('dashboard.gateway') }}</span>
        </div>
        <div class="stat-card__value">{{ data?.metrics.activeProxies ?? 0 }}</div>
        <div class="stat-card__label">{{ t('dashboard.activeProxies') }}</div>
        <div class="stat-card__foot"><span>{{ t('dashboard.proxied', { count: data?.metrics.proxyRequests ?? 0 }) }}</span><span class="mono">{{ t('dashboard.errors', { count: data?.metrics.proxyErrors ?? 0 }) }}</span></div>
      </article>
      <article class="stat-card">
        <div class="stat-card__top">
          <span class="stat-card__icon"><Icon name="pulse" :size="18" /></span>
          <span class="stat-card__trend">{{ t('dashboard.transport') }}</span>
        </div>
        <div class="stat-card__value">{{ (data?.metrics.tcpProbes ?? 0) + (data?.metrics.udpProbes ?? 0) }}</div>
        <div class="stat-card__label">{{ t('dashboard.transportProbes') }}</div>
        <div class="stat-card__foot"><span>{{ t('dashboard.tcpCount', { count: data?.metrics.tcpProbes ?? 0 }) }}</span><span class="mono">{{ t('dashboard.udpCount', { count: data?.metrics.udpProbes ?? 0 }) }}</span></div>
      </article>
      <article class="stat-card">
        <div class="stat-card__top">
          <span class="stat-card__icon"><Icon name="clock" :size="18" /></span>
          <span class="stat-card__trend">{{ t('dashboard.process') }}</span>
        </div>
        <div class="stat-card__value stat-card__value--small">{{ uptime }}</div>
        <div class="stat-card__label">{{ t('dashboard.uptime') }}</div>
        <div class="stat-card__foot"><span>{{ t('dashboard.rss', { value: rss }) }}</span><span class="mono">v{{ data?.server.version }}</span></div>
      </article>
    </section>

    <section class="dash-grid">
      <article class="card card--chart">
        <header class="card__head">
          <div>
            <span class="eyebrow">{{ t('dashboard.telemetry') }}</span>
            <h3>{{ t('dashboard.latencyProfile') }}</h3>
          </div>
          <div class="card__tools">
            <span class="muted mono">{{ lastUpdated ? lastUpdated.toLocaleTimeString() : t('dashboard.waiting') }}</span>
            <button class="icon-button" :class="{ spinning: refreshing }" :title="t('dashboard.refresh')" @click="load(false)">
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
            <div class="chart__axis"><span>{{ t('dashboard.older') }}</span><span class="mono">{{ t('dashboard.peak', { value: sparkline.max }) }}</span><span>{{ t('dashboard.now') }}</span></div>
          </div>
          <div v-else class="empty-inline">
            <Icon name="activity" :size="20" />
            <span>{{ t('dashboard.emptyLatency') }}</span>
          </div>
        </div>
        <div class="metric-row">
          <div><span>{{ t('dashboard.successRate') }}</span><strong>{{ successRate }}%</strong></div>
          <div><span>{{ t('dashboard.failures') }}</span><strong>{{ data?.metrics.httpErrors ?? 0 }}</strong></div>
          <div><span>{{ t('dashboard.tlsInspections') }}</span><strong>{{ data?.metrics.tlsInspections ?? 0 }}</strong></div>
          <div><span>{{ t('dashboard.users') }}</span><strong>{{ data?.metrics.users ?? 0 }}</strong></div>
        </div>
      </article>

      <article class="card">
        <header class="card__head">
          <div>
            <span class="eyebrow">{{ t('dashboard.mix') }}</span>
            <h3>{{ t('dashboard.protocolActivity') }}</h3>
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
            <span><i class="status-dot" />{{ t('dashboard.daemon') }}</span><strong>{{ t('dashboard.healthy') }}</strong>
          </div>
          <div class="health-row">
            <span><i class="status-dot status-dot--blue" />{{ t('dashboard.database') }}</span><strong>SQLite · WAL</strong>
          </div>
          <div class="health-row">
            <span><i class="status-dot status-dot--violet" />{{ t('dashboard.gateway') }}</span><strong>{{ t('dashboard.running', { count: data?.metrics.activeProxies ?? 0 }) }}</strong>
          </div>
        </div>
      </article>

      <article class="card">
        <header class="card__head">
          <div>
            <span class="eyebrow">{{ t('dashboard.shortcuts') }}</span>
            <h3>{{ t('dashboard.openWorkbench') }}</h3>
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
            <span class="eyebrow">{{ t('dashboard.timeline') }}</span>
            <h3>{{ t('dashboard.recentActivity') }}</h3>
          </div>
          <button class="link-button" @click="go('/settings')">{{ t('dashboard.history') }}</button>
        </header>
        <div v-if="recent.length" class="activity-list">
          <div v-for="item in recent" :key="item.id" class="activity-row">
            <span class="kind-chip" :class="`kind-chip--${item.kind}`">{{ item.kind }}</span>
            <div class="activity-row__main">
              <strong>{{ item.summary }}</strong>
              <span>{{ formatTime(item.createdAt) }} · {{ item.latencyMs }} ms · {{ formatBytes(item.sizeBytes) }}</span>
            </div>
            <span class="status-code" :class="item.status === 'success' ? 'status-code--ok' : 'status-code--bad'">
              {{ item.httpStatus || (item.status === 'success' ? t('dashboard.statusOk') : t('dashboard.statusErr')) }}
            </span>
          </div>
        </div>
        <div v-else class="empty-inline">
          <Icon name="terminal" :size="20" />
          <span>{{ t('dashboard.emptyActivity') }}</span>
        </div>
      </article>
    </section>
  </div>
</template>
