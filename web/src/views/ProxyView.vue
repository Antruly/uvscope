<script setup lang="ts">
import { computed, onActivated, onBeforeUnmount, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { del, errorMessage, get, post } from '../api'
import type { ProxyInfo, ProxyLog } from '../types'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'ProxyView' })

const auth = useAuthStore()
const ui = useUiStore()
const proxies = ref<ProxyInfo[]>([])
const loading = ref(true)
const logs = ref<ProxyLog[]>([])
const logsFor = ref<string | null>(null)
const logsLoading = ref(false)
const editorOpen = ref(false)
const saving = ref(false)
const formError = ref('')
const logEl = ref<HTMLElement | null>(null)
let logsTimer: number | undefined
let lastSeq = 0

const blank = () => ({
  id: '',
  name: '',
  listenHost: '127.0.0.1',
  listenPort: 18080,
  target: 'http://127.0.0.1:8000',
  autoStart: false,
  tls: false,
  certPath: '',
  keyPath: '',
})
const form = ref(blank())

const canOperate = computed(() => auth.user?.role === 'admin' || auth.user?.role === 'operator')
const activeCount = computed(() => proxies.value.filter((proxy) => proxy.running).length)
const totalRequests = computed(() => proxies.value.reduce((total, proxy) => total + proxy.requests, 0))
const totalErrors = computed(() => proxies.value.reduce((total, proxy) => total + proxy.errors, 0))
const selectedProxy = computed(() => proxies.value.find((proxy) => proxy.id === logsFor.value) || null)

async function load() {
  loading.value = true
  try {
    const response = await get<{ proxies: ProxyInfo[] }>('/api/proxies')
    proxies.value = response.proxies
  } catch (err) {
    ui.toastError(err, 'Could not load proxies')
  } finally {
    loading.value = false
  }
}

function openCreate() {
  form.value = blank()
  formError.value = ''
  editorOpen.value = true
}

function openEdit(proxy: ProxyInfo) {
  form.value = {
    id: proxy.id,
    name: proxy.name,
    listenHost: proxy.listenHost,
    listenPort: proxy.listenPort,
    target: proxy.target,
    autoStart: proxy.autoStart,
    tls: proxy.tls,
    certPath: proxy.certPath || '',
    keyPath: proxy.keyPath || '',
  }
  formError.value = ''
  editorOpen.value = true
}

async function save(start = false) {
  formError.value = ''
  if (!form.value.name.trim()) {
    formError.value = 'Give this proxy a name.'
    return
  }
  if (!form.value.listenPort) {
    formError.value = 'A listen port is required.'
    return
  }
  if (!/^https?:\/\//.test(form.value.target)) {
    formError.value = 'Target must start with http:// or https://.'
    return
  }
  if (form.value.tls && (!form.value.certPath || !form.value.keyPath)) {
    formError.value = 'TLS listener requires both a certificate and private key path.'
    return
  }
  saving.value = true
  try {
    await post('/api/proxies', { ...form.value, name: form.value.name.trim(), start })
    editorOpen.value = false
    await load()
    ui.notify('success', form.value.id ? 'Proxy updated' : 'Proxy created')
  } catch (err) {
    formError.value = errorMessage(err)
  } finally {
    saving.value = false
  }
}

async function toggle(proxy: ProxyInfo) {
  try {
    await post(`/api/proxies/${proxy.id}/${proxy.running ? 'stop' : 'start'}`)
    await load()
    ui.notify('success', proxy.running ? 'Proxy stopped' : 'Proxy started', proxy.name)
  } catch (err) {
    ui.toastError(err, proxy.running ? 'Could not stop proxy' : 'Could not start proxy')
  }
}

async function remove(proxy: ProxyInfo) {
  if (!window.confirm(`Delete reverse proxy “${proxy.name}”? Its listener will be stopped.`)) return
  try {
    await del(`/api/proxies/${proxy.id}`)
    if (logsFor.value === proxy.id) closeLogs()
    await load()
    ui.notify('success', 'Proxy deleted')
  } catch (err) {
    ui.toastError(err)
  }
}

async function showLogs(proxy: ProxyInfo) {
  logsFor.value = proxy.id
  logs.value = []
  lastSeq = 0
  await loadLogs()
  startLogPolling()
  window.setTimeout(() => logEl.value?.scrollTo({ top: logEl.value.scrollHeight }), 50)
}

function closeLogs() {
  logsFor.value = null
  logs.value = []
  stopLogPolling()
}

async function loadLogs() {
  if (!logsFor.value) return
  logsLoading.value = true
  try {
    const response = await get<{ logs: ProxyLog[] }>(`/api/proxies/${logsFor.value}/logs?after=${lastSeq}&limit=200`)
    if (response.logs.length) {
      logs.value.push(...response.logs)
      lastSeq = response.logs[response.logs.length - 1].seq
      if (logs.value.length > 600) logs.value.splice(0, logs.value.length - 600)
      requestAnimationFrame(() => {
        if (logEl.value) logEl.value.scrollTop = logEl.value.scrollHeight
      })
    }
  } catch {
    // Logs are a live convenience; reconnect on the next poll.
  } finally {
    logsLoading.value = false
  }
}

function startLogPolling() {
  stopLogPolling()
  logsTimer = window.setInterval(() => void loadLogs(), 1800)
}
function stopLogPolling() {
  if (logsTimer) window.clearInterval(logsTimer)
  logsTimer = undefined
}

function statusLabel(proxy: ProxyInfo) {
  if (proxy.running) return 'running'
  if (proxy.lastError) return 'error'
  return 'stopped'
}

function formatTime(value: string) {
  return new Date(value).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

onMounted(load)
onActivated(load)
onBeforeUnmount(stopLogPolling)
</script>

<template>
  <div class="page proxy-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">Gateway</span>
        <h2>Put traffic on a leash.</h2>
        <p>Launch HTTP reverse proxies on this machine, watch requests, and stop them without touching a config file.</p>
      </div>
      <div class="page-head__actions">
        <button class="button button--primary" :disabled="!canOperate" @click="openCreate">
          <Icon name="plus" :size="16" />New proxy
        </button>
      </div>
    </header>

    <section class="proxy-stats">
      <div class="proxy-stat"><span class="proxy-stat__icon"><Icon name="route" :size="18" /></span><div><strong>{{ activeCount }}</strong><span>active listeners</span></div></div>
      <div class="proxy-stat"><span class="proxy-stat__icon"><Icon name="activity" :size="18" /></span><div><strong>{{ totalRequests }}</strong><span>proxied requests</span></div></div>
      <div class="proxy-stat"><span class="proxy-stat__icon"><Icon name="alert" :size="18" /></span><div><strong>{{ totalErrors }}</strong><span>upstream errors</span></div></div>
      <div class="proxy-stat"><span class="proxy-stat__icon"><Icon name="globe" :size="18" /></span><div><strong>{{ proxies.length }}</strong><span>configured routes</span></div></div>
    </section>

    <div v-if="!canOperate" class="inline-alert inline-alert--warning">
      <Icon name="lock" :size="16" />Your role can view proxy state but cannot start or change listeners.
    </div>

    <div v-if="loading" class="proxy-loading"><span class="spinner" />Loading gateway…</div>
    <div v-else-if="!proxies.length" class="empty-state">
      <div class="empty-state__art"><Icon name="route" :size="34" /></div>
      <h3>No reverse proxies yet</h3>
      <p>Point a local port at an upstream service. uvscope handles the listener, request forwarding and live trace.</p>
      <button class="button button--primary" :disabled="!canOperate" @click="openCreate"><Icon name="plus" :size="16" />Create the first proxy</button>
    </div>
    <div v-else class="proxy-grid">
      <article v-for="proxy in proxies" :key="proxy.id" class="proxy-card card" :class="{ 'proxy-card--running': proxy.running }">
        <div class="proxy-card__top">
          <span class="proxy-card__status" :class="`proxy-card__status--${statusLabel(proxy)}`"><i class="status-dot" />{{ statusLabel(proxy) }}</span>
          <div class="proxy-card__tools">
            <button class="icon-button icon-button--sm" title="Edit proxy" :disabled="!canOperate" @click="openEdit(proxy)"><Icon name="edit" :size="15" /></button>
            <button class="icon-button icon-button--sm icon-button--danger" title="Delete proxy" :disabled="!canOperate" @click="remove(proxy)"><Icon name="trash" :size="15" /></button>
          </div>
        </div>
        <div class="proxy-card__title">
          <h3>{{ proxy.name }}</h3>
          <span v-if="proxy.tls" class="chip chip--accent"><Icon name="lock" :size="12" />TLS</span>
          <span v-if="proxy.autoStart" class="chip">auto</span>
        </div>
        <div class="proxy-route">
          <div><span>Listen</span><code class="mono">{{ proxy.listenHost }}:{{ proxy.listenPort }}</code></div>
          <div class="proxy-route__arrow"><Icon name="arrow-right" :size="15" /></div>
          <div><span>Upstream</span><code class="mono">{{ proxy.target }}</code></div>
        </div>
        <div class="proxy-card__metrics">
          <div><strong>{{ proxy.requests }}</strong><span>requests</span></div>
          <div><strong>{{ proxy.errors }}</strong><span>errors</span></div>
          <div><strong>{{ proxy.bytes || 0 }}</strong><span>bytes</span></div>
        </div>
        <div v-if="proxy.lastError" class="proxy-error"><Icon name="alert" :size="14" />{{ proxy.lastError }}</div>
        <div class="proxy-card__actions">
          <button class="button" :class="{ 'button--danger': proxy.running }" :disabled="!canOperate" @click="toggle(proxy)">
            <Icon :name="proxy.running ? 'stop' : 'play'" :size="15" />{{ proxy.running ? 'Stop' : 'Start' }}
          </button>
          <button class="button button--ghost" @click="showLogs(proxy)"><Icon name="terminal" :size="15" />Logs</button>
        </div>
      </article>
    </div>

    <Transition name="reveal">
      <section v-if="selectedProxy" class="card proxy-logs">
        <header class="card__head">
          <div><span class="eyebrow">Live trace</span><h3>{{ selectedProxy.name }} · {{ selectedProxy.listenHost }}:{{ selectedProxy.listenPort }}</h3></div>
          <div class="card__tools">
            <span v-if="logsLoading" class="spinner spinner--sm" />
            <button class="icon-button icon-button--sm" title="Close logs" @click="closeLogs"><Icon name="close" :size="16" /></button>
          </div>
        </header>
        <div ref="logEl" class="proxy-log">
          <div v-if="!logs.length" class="history-empty"><Icon name="terminal" :size="18" />No proxy events yet.</div>
          <div v-for="entry in logs" :key="entry.seq" class="proxy-log__row" :class="{ 'proxy-log__row--error': entry.error || entry.status >= 500 }">
            <span class="mono proxy-log__time">{{ formatTime(entry.time) }}</span>
            <span class="kind-chip" :class="entry.method === 'SYSTEM' ? 'kind-chip--tls' : 'kind-chip--http'">{{ entry.method }}</span>
            <span class="proxy-log__path mono">{{ entry.path }}</span>
            <span class="status-code" :class="entry.status >= 400 || entry.error ? 'status-code--bad' : entry.status ? 'status-code--ok' : ''">{{ entry.status || (entry.error ? 'ERR' : '·') }}</span>
            <span class="mono proxy-log__latency">{{ entry.latencyMs }} ms</span>
            <span v-if="entry.error" class="proxy-log__error">{{ entry.error }}</span>
            <span v-else-if="entry.message" class="proxy-log__message">{{ entry.message }}</span>
            <span v-else class="mono proxy-log__latency">{{ entry.bytes || 0 }} B</span>
          </div>
        </div>
      </section>
    </Transition>

    <Teleport to="body">
      <Transition name="modal">
        <div v-if="editorOpen" class="modal-scrim" @click.self="editorOpen = false">
          <form class="modal-card" @submit.prevent="save(true)">
            <header class="modal-head">
              <div><span class="eyebrow">Reverse proxy</span><h3>{{ form.id ? 'Edit listener' : 'New listener' }}</h3></div>
              <button type="button" class="icon-button" @click="editorOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <label class="field"><span>Name</span><input v-model="form.name" placeholder="Staging API" autofocus /></label>
              <div class="form-grid">
                <label class="field"><span>Listen host</span><input v-model="form.listenHost" class="mono" /></label>
                <label class="field"><span>Listen port</span><input v-model.number="form.listenPort" type="number" min="1" max="65535" class="mono" /></label>
              </div>
              <label class="field"><span>Upstream target</span><input v-model="form.target" class="mono" placeholder="http://127.0.0.1:8000" /></label>
              <div class="switch-row">
                <label class="switch-field"><span>Start automatically</span><button type="button" class="switch" :class="{ on: form.autoStart }" @click="form.autoStart = !form.autoStart"><i /></button></label>
                <label class="switch-field"><span>HTTPS listener</span><button type="button" class="switch" :class="{ on: form.tls }" @click="form.tls = !form.tls"><i /></button></label>
              </div>
              <div v-if="form.tls" class="form-grid">
                <label class="field"><span>Certificate path</span><input v-model="form.certPath" class="mono" placeholder="/etc/ssl/certs/site.pem" /></label>
                <label class="field"><span>Private key path</span><input v-model="form.keyPath" class="mono" placeholder="/etc/ssl/private/site.key" /></label>
              </div>
              <div v-if="formError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ formError }}</div>
              <p class="modal-hint">HTTP/1.1 forwarding only in 0.1.0. WebSocket upgrade and per-path rules are on the roadmap.</p>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="editorOpen = false">Cancel</button>
              <button type="submit" class="button button--primary" :disabled="saving">
                <span v-if="saving" class="spinner" /><Icon v-else name="play" :size="15" />Save & start
              </button>
            </footer>
          </form>
        </div>
      </Transition>
    </Teleport>
  </div>
</template>
