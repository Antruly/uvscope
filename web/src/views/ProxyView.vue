<script setup lang="ts">
import { computed, onActivated, onBeforeUnmount, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { del, errorMessage, get, post } from '../api'
import type { ProxyInfo, ProxyLog, ProxyRule, ProxyUpstream } from '../types'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'

defineOptions({ name: 'ProxyView' })

const auth = useAuthStore()
const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
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

function blankRule(): ProxyRule {
  return {
    id: '',
    name: '',
    enabled: true,
    matchType: 'prefix',
    pattern: '',
    method: '',
    host: '',
    headerName: '',
    headerValue: '',
    action: 'redirect',
    status: 302,
    destination: '',
    preserveQuery: true,
  }
}

const blank = () => ({
  id: '',
  name: '',
  listenHost: '127.0.0.1',
  listenPort: 18080,
  target: 'http://127.0.0.1:8000',
  upstreams: [] as ProxyUpstream[],
  primaryWeight: 1,
  retries: 0,
  autoStart: false,
  tls: false,
  certPath: '',
  keyPath: '',
  rules: [] as ProxyRule[],
})
const form = ref(blank())
const managedCerts = ref<Array<{ id: string; name: string; certPath: string; keyPath: string; notAfter: string }>>([])
const selectedCertId = ref('')

async function loadManagedCerts() {
  try {
    const response = await get<{ ok: boolean; certificates: Array<{ id: string; name: string; certPath: string; keyPath: string; notAfter: string }> }>(
      '/api/tls/certificates',
    )
    if (response.ok) managedCerts.value = response.certificates
  } catch {
    managedCerts.value = []
  }
}

function applyManagedCert() {
  const cert = managedCerts.value.find((item) => item.id === selectedCertId.value)
  if (!cert) return
  form.value.certPath = cert.certPath
  form.value.keyPath = cert.keyPath
}

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
    ui.toastError(err, t('proxy.error.load'))
  } finally {
    loading.value = false
  }
}

function openCreate() {
  form.value = blank()
  selectedCertId.value = ''
  formError.value = ''
  editorOpen.value = true
  void loadManagedCerts()
}

function openEdit(proxy: ProxyInfo) {
  const upstreams = (proxy.upstreams || []).map((item) => ({ ...item }))
  const primary = upstreams.shift()
  form.value = {
    id: proxy.id,
    name: proxy.name,
    listenHost: proxy.listenHost,
    listenPort: proxy.listenPort,
    target: primary ? primary.url : proxy.target,
    upstreams,
    primaryWeight: primary ? primary.weight : 1,
    retries: proxy.retries || 0,
    autoStart: proxy.autoStart,
    tls: proxy.tls,
    certPath: proxy.certPath || '',
    keyPath: proxy.keyPath || '',
    rules: (proxy.rules || []).map((rule) => ({ ...rule })),
  }
  selectedCertId.value = ''
  formError.value = ''
  editorOpen.value = true
  void loadManagedCerts().then(() => {
    const match = managedCerts.value.find(
      (item) => item.certPath === form.value.certPath && item.keyPath === form.value.keyPath,
    )
    if (match) selectedCertId.value = match.id
  })
}

function addUpstream() {
  form.value.upstreams.push({ url: '', weight: 1 })
}

function removeUpstream(index: number) {
  form.value.upstreams.splice(index, 1)
}

function unhealthyCount(proxy: ProxyInfo): number {
  return (proxy.health || []).filter((item) => item.checked && !item.healthy).length
}

function addRule() {
  form.value.rules.push(blankRule())
}

function removeRule(index: number) {
  form.value.rules.splice(index, 1)
}

async function save(start = false) {
  formError.value = ''
  if (!form.value.name.trim()) {
    formError.value = t('proxy.error.nameRequired')
    return
  }
  if (!form.value.listenPort) {
    formError.value = t('proxy.error.portRequired')
    return
  }
  if (!/^https?:\/\//.test(form.value.target)) {
    formError.value = t('proxy.error.targetScheme')
    return
  }
  if (form.value.tls && (!form.value.certPath || !form.value.keyPath)) {
    formError.value = t('proxy.error.tlsPaths')
    return
  }
  for (const [index, upstream] of form.value.upstreams.entries()) {
    if (!/^https?:\/\//.test(upstream.url.trim())) {
      formError.value = t('proxy.upstreams.invalid', { index: index + 2 })
      return
    }
    if (!upstream.weight || upstream.weight < 1 || upstream.weight > 1000) {
      formError.value = t('proxy.upstreams.weightError', { index: index + 2 })
      return
    }
  }
  for (const [index, rule] of form.value.rules.entries()) {
    const hasCondition = Boolean(
      rule.pattern.trim() || rule.method?.trim() || rule.host?.trim() || rule.headerName?.trim(),
    )
    if (!hasCondition || !rule.destination.trim()) {
      formError.value = t('proxy.rules.incomplete', { index: index + 1 })
      return
    }
  }
  saving.value = true
  try {
    const upstreams = [
      { url: form.value.target.trim(), weight: form.value.primaryWeight || 1 },
      ...form.value.upstreams
        .filter((upstream) => upstream.url.trim())
        .map((upstream) => ({ url: upstream.url.trim(), weight: upstream.weight || 1 })),
    ]
    const response = await post<{ ok: boolean; warning?: string; error?: string }>(
      '/api/proxies',
      { ...form.value, upstreams, name: form.value.name.trim(), start },
    )
    editorOpen.value = false
    await load()
    if (response.warning) {
      ui.notify('error', t('proxy.error.start'), response.warning)
      return
    }
    ui.notify('success', form.value.id ? t('proxy.notify.updated') : t('proxy.notify.created'))
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
    ui.notify('success', proxy.running ? t('proxy.notify.stopped') : t('proxy.notify.started'), proxy.name)
  } catch (err) {
    ui.toastError(err, proxy.running ? t('proxy.error.stop') : t('proxy.error.start'))
  }
}

async function remove(proxy: ProxyInfo) {
  if (!window.confirm(t('proxy.confirm.delete', { name: proxy.name }))) return
  try {
    await del(`/api/proxies/${proxy.id}`)
    if (logsFor.value === proxy.id) closeLogs()
    await load()
    ui.notify('success', t('proxy.notify.deleted'))
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

function statusKey(proxy: ProxyInfo) {
  if (proxy.running) return 'running'
  if (proxy.lastError) return 'error'
  return 'stopped'
}

function statusLabel(proxy: ProxyInfo) {
  return t(`proxy.status.${statusKey(proxy)}`)
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
        <span class="eyebrow">{{ t('proxy.eyebrow') }}</span>
        <h2>{{ t('proxy.title') }}</h2>
        <p>{{ t('proxy.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <button class="button button--primary" :disabled="!canOperate" @click="openCreate">
          <Icon name="plus" :size="16" />{{ t('proxy.new') }}
        </button>
      </div>
    </header>

    <section class="proxy-stats">
      <div class="proxy-stat"><span class="proxy-stat__icon"><Icon name="route" :size="18" /></span><div><strong>{{ activeCount }}</strong><span>{{ t('proxy.stats.active') }}</span></div></div>
      <div class="proxy-stat"><span class="proxy-stat__icon"><Icon name="activity" :size="18" /></span><div><strong>{{ totalRequests }}</strong><span>{{ t('proxy.stats.requests') }}</span></div></div>
      <div class="proxy-stat"><span class="proxy-stat__icon"><Icon name="alert" :size="18" /></span><div><strong>{{ totalErrors }}</strong><span>{{ t('proxy.stats.errors') }}</span></div></div>
      <div class="proxy-stat"><span class="proxy-stat__icon"><Icon name="globe" :size="18" /></span><div><strong>{{ proxies.length }}</strong><span>{{ t('proxy.stats.routes') }}</span></div></div>
    </section>

    <div v-if="!canOperate" class="inline-alert inline-alert--warning">
      <Icon name="lock" :size="16" />{{ t('proxy.readOnly') }}
    </div>

    <div v-if="loading" class="proxy-loading"><span class="spinner" />{{ t('proxy.loading') }}</div>
    <div v-else-if="!proxies.length" class="empty-state">
      <div class="empty-state__art"><Icon name="route" :size="34" /></div>
      <h3>{{ t('proxy.empty.title') }}</h3>
      <p>{{ t('proxy.empty.body') }}</p>
      <button class="button button--primary" :disabled="!canOperate" @click="openCreate"><Icon name="plus" :size="16" />{{ t('proxy.empty.action') }}</button>
    </div>
    <div v-else class="proxy-grid">
      <article v-for="proxy in proxies" :key="proxy.id" class="proxy-card card" :class="{ 'proxy-card--running': proxy.running }">
        <div class="proxy-card__top">
          <span class="proxy-card__status" :class="`proxy-card__status--${statusKey(proxy)}`"><i class="status-dot" />{{ statusLabel(proxy) }}</span>
          <div class="proxy-card__tools">
            <button class="icon-button icon-button--sm" :title="t('proxy.card.edit')" :disabled="!canOperate" @click="openEdit(proxy)"><Icon name="edit" :size="15" /></button>
            <button class="icon-button icon-button--sm icon-button--danger" :title="t('proxy.card.delete')" :disabled="!canOperate" @click="remove(proxy)"><Icon name="trash" :size="15" /></button>
          </div>
        </div>
        <div class="proxy-card__title">
          <h3>{{ proxy.name }}</h3>
          <span v-if="proxy.tls" class="chip chip--accent"><Icon name="lock" :size="12" />TLS</span>
          <span v-if="proxy.autoStart" class="chip">{{ t('proxy.card.auto') }}</span>
          <span v-if="proxy.rules?.length" class="chip">{{ t('proxy.card.rules', { count: proxy.rules.length }) }}</span>
        </div>
        <div class="proxy-route">
          <div><span>{{ t('proxy.card.listen') }}</span><code class="mono">{{ proxy.listenHost }}:{{ proxy.listenPort }}</code></div>
          <div class="proxy-route__arrow"><Icon name="arrow-right" :size="15" /></div>
          <div><span>{{ t('proxy.card.upstream') }}</span><code class="mono">{{ proxy.target }}</code></div>
        </div>
        <div class="proxy-card__metrics">
          <div><strong>{{ proxy.requests }}</strong><span>{{ t('proxy.card.requests') }}</span></div>
          <div><strong>{{ proxy.errors }}</strong><span>{{ t('proxy.card.errors') }}</span></div>
          <div><strong>{{ proxy.bytes || 0 }}</strong><span>{{ t('proxy.card.bytes') }}</span></div>
          <div><strong>{{ (proxy.metrics?.qps ?? 0).toFixed(1) }}</strong><span>{{ t('proxy.card.qps') }}</span></div>
          <div><strong>{{ proxy.metrics?.p50Ms ?? 0 }} / {{ proxy.metrics?.p95Ms ?? 0 }} ms</strong><span>{{ t('proxy.card.latency') }}</span></div>
        </div>
        <div v-if="proxy.lastError" class="proxy-error"><Icon name="alert" :size="14" />{{ proxy.lastError }}</div>
        <div v-if="unhealthyCount(proxy)" class="proxy-error"><Icon name="alert" :size="14" />{{ t('proxy.card.unhealthy', { count: unhealthyCount(proxy) }) }}</div>
        <div class="proxy-card__actions">
          <button class="button" :class="{ 'button--danger': proxy.running }" :disabled="!canOperate" @click="toggle(proxy)">
            <Icon :name="proxy.running ? 'stop' : 'play'" :size="15" />{{ proxy.running ? t('proxy.card.stop') : t('proxy.card.start') }}
          </button>
          <button class="button button--ghost" @click="showLogs(proxy)"><Icon name="terminal" :size="15" />{{ t('proxy.card.logs') }}</button>
        </div>
      </article>
    </div>

    <Transition name="reveal">
      <section v-if="selectedProxy" class="card proxy-logs">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('proxy.logs.eyebrow') }}</span><h3>{{ selectedProxy.name }} · {{ selectedProxy.listenHost }}:{{ selectedProxy.listenPort }}</h3></div>
          <div class="card__tools">
            <span v-if="logsLoading" class="spinner spinner--sm" />
            <button class="icon-button icon-button--sm" :title="t('proxy.logs.close')" @click="closeLogs"><Icon name="close" :size="16" /></button>
          </div>
        </header>
        <div ref="logEl" class="proxy-log">
          <div v-if="!logs.length" class="history-empty"><Icon name="terminal" :size="18" />{{ t('proxy.logs.empty') }}</div>
          <div v-for="entry in logs" :key="entry.seq" class="proxy-log__row" :class="{ 'proxy-log__row--error': entry.error || entry.status >= 500 }">
            <span class="mono proxy-log__time">{{ formatTime(entry.time) }}</span>
            <span class="kind-chip" :class="entry.method === 'SYSTEM' ? 'kind-chip--tls' : 'kind-chip--http'">{{ entry.method }}</span>
            <span class="proxy-log__path mono">{{ entry.path }}</span>
            <span class="status-code" :class="entry.status >= 400 || entry.error ? 'status-code--bad' : entry.status ? 'status-code--ok' : ''">{{ entry.status || (entry.error ? t('proxy.logs.errorCode') : '·') }}</span>
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
          <form class="modal-card modal-card--wide" @submit.prevent="save(true)">
            <header class="modal-head">
              <div><span class="eyebrow">{{ t('proxy.editor.eyebrow') }}</span><h3>{{ form.id ? t('proxy.editor.editTitle') : t('proxy.editor.newTitle') }}</h3></div>
              <button type="button" class="icon-button" @click="editorOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <label class="field"><span>{{ t('proxy.editor.name') }}</span><input v-model="form.name" :placeholder="t('proxy.editor.namePlaceholder')" autofocus /></label>
              <div class="form-grid">
                <label class="field"><span>{{ t('proxy.editor.listenHost') }}</span><input v-model="form.listenHost" class="mono" /></label>
                <label class="field"><span>{{ t('proxy.editor.listenPort') }}</span><input v-model.number="form.listenPort" type="number" min="1" max="65535" class="mono" /></label>
              </div>
              <div class="form-grid">
                <label class="field field--grow"><span>{{ t('proxy.editor.target') }}</span><input v-model="form.target" class="mono" placeholder="http://127.0.0.1:8000" /></label>
                <label class="field"><span>{{ t('proxy.upstreams.weight') }}</span><input v-model.number="form.primaryWeight" type="number" min="1" max="1000" class="mono" /></label>
              </div>
              <div class="proxy-rules-editor">
                <div class="proxy-rules-editor__head">
                  <div>
                    <strong>{{ t('proxy.upstreams.title') }}</strong>
                    <p class="muted">{{ t('proxy.upstreams.hint') }}</p>
                  </div>
                  <button type="button" class="link-button" @click="addUpstream"><Icon name="plus" :size="14" />{{ t('proxy.upstreams.add') }}</button>
                </div>
                <div v-if="!form.upstreams.length" class="proxy-rules-empty">{{ t('proxy.upstreams.empty') }}</div>
                <div v-for="(upstream, index) in form.upstreams" :key="index" class="kv-row">
                  <input v-model="upstream.url" class="mono" placeholder="http://127.0.0.1:8001" spellcheck="false" />
                  <input v-model.number="upstream.weight" type="number" min="1" max="1000" class="mono" />
                  <button type="button" class="icon-button icon-button--sm icon-button--danger" :title="t('proxy.upstreams.remove')" @click="removeUpstream(index)"><Icon name="trash" :size="14" /></button>
                </div>
                <label class="field"><span>{{ t('proxy.editor.retries') }}</span><input v-model.number="form.retries" type="number" min="0" max="3" class="mono" /></label>
              </div>
              <div class="switch-row">
                <label class="switch-field"><span>{{ t('proxy.editor.autoStart') }}</span><button type="button" class="switch" :class="{ on: form.autoStart }" @click="form.autoStart = !form.autoStart"><i /></button></label>
                <label class="switch-field"><span>{{ t('proxy.editor.tls') }}</span><button type="button" class="switch" :class="{ on: form.tls }" @click="form.tls = !form.tls"><i /></button></label>
              </div>
              <label v-if="form.tls" class="field">
                <span>{{ t('proxy.editor.managedCert') }}</span>
                <select v-model="selectedCertId" class="select" @change="applyManagedCert">
                  <option value="">{{ t('proxy.editor.customCert') }}</option>
                  <option v-for="cert in managedCerts" :key="cert.id" :value="cert.id">{{ cert.name }} · {{ cert.notAfter }}</option>
                </select>
              </label>
              <div v-if="form.tls" class="form-grid">
                <label class="field"><span>{{ t('proxy.editor.certPath') }}</span><input v-model="form.certPath" class="mono" placeholder="/etc/ssl/certs/site.pem" /></label>
                <label class="field"><span>{{ t('proxy.editor.keyPath') }}</span><input v-model="form.keyPath" class="mono" placeholder="/etc/ssl/private/site.key" /></label>
              </div>

              <div class="proxy-rules-editor">
                <div class="proxy-rules-editor__head">
                  <div>
                    <strong>{{ t('proxy.rules.title') }}</strong>
                    <p class="muted">{{ t('proxy.rules.hint') }}</p>
                  </div>
                  <button type="button" class="link-button" @click="addRule">
                    <Icon name="plus" :size="14" />{{ t('proxy.rules.add') }}
                  </button>
                </div>
                <div v-if="!form.rules.length" class="proxy-rules-empty">{{ t('proxy.rules.empty') }}</div>
                <div v-for="(rule, index) in form.rules" :key="index" class="proxy-rule">
                  <div class="proxy-rule__head">
                    <label class="listener-check">
                      <input v-model="rule.enabled" type="checkbox" />{{ t('proxy.rules.enabled') }}
                    </label>
                    <input v-model="rule.name" class="proxy-rule__name" :placeholder="t('proxy.rules.namePlaceholder')" />
                    <button type="button" class="icon-button icon-button--sm icon-button--danger" :title="t('proxy.rules.remove')" @click="removeRule(index)">
                      <Icon name="trash" :size="14" />
                    </button>
                  </div>
                  <div class="form-grid form-grid--tight">
                    <label class="field">
                      <span>{{ t('proxy.rules.match') }}</span>
                      <select v-model="rule.matchType" class="select">
                        <option value="exact">{{ t('proxy.rules.match.exact') }}</option>
                        <option value="prefix">{{ t('proxy.rules.match.prefix') }}</option>
                        <option value="regex">{{ t('proxy.rules.match.regex') }}</option>
                      </select>
                    </label>
                    <label class="field field--grow">
                      <span>{{ t('proxy.rules.pattern') }}</span>
                      <input v-model="rule.pattern" class="mono" :placeholder="t('proxy.rules.patternPlaceholder')" spellcheck="false" />
                    </label>
                  </div>
                  <div class="form-grid form-grid--tight">
                    <label class="field">
                      <span>{{ t('proxy.rules.method') }}</span>
                      <input v-model="rule.method" class="mono" placeholder="GET,POST" spellcheck="false" />
                    </label>
                    <label class="field field--grow">
                      <span>{{ t('proxy.rules.host') }}</span>
                      <input v-model="rule.host" class="mono" placeholder="api.example.com / *.example.com" spellcheck="false" />
                    </label>
                  </div>
                  <div class="form-grid form-grid--tight">
                    <label class="field">
                      <span>{{ t('proxy.rules.headerName') }}</span>
                      <input v-model="rule.headerName" class="mono" placeholder="Authorization" spellcheck="false" />
                    </label>
                    <label class="field field--grow">
                      <span>{{ t('proxy.rules.headerValue') }}</span>
                      <input v-model="rule.headerValue" class="mono" :placeholder="t('proxy.rules.headerValuePlaceholder')" spellcheck="false" />
                    </label>
                  </div>
                  <div class="form-grid form-grid--tight">
                    <label class="field">
                      <span>{{ t('proxy.rules.action') }}</span>
                      <select v-model="rule.action" class="select">
                        <option value="redirect">{{ t('proxy.rules.action.redirect') }}</option>
                        <option value="rewrite">{{ t('proxy.rules.action.rewrite') }}</option>
                      </select>
                    </label>
                    <label v-if="rule.action === 'redirect'" class="field">
                      <span>{{ t('proxy.rules.status') }}</span>
                      <select v-model.number="rule.status" class="select">
                        <option :value="301">301</option>
                        <option :value="302">302</option>
                        <option :value="307">307</option>
                        <option :value="308">308</option>
                      </select>
                    </label>
                    <label class="field field--grow">
                      <span>{{ t('proxy.rules.destination') }}</span>
                      <input v-model="rule.destination" class="mono" :placeholder="t('proxy.rules.destinationPlaceholder')" spellcheck="false" />
                    </label>
                  </div>
                  <label class="listener-check">
                    <input v-model="rule.preserveQuery" type="checkbox" />{{ t('proxy.rules.preserveQuery') }}
                  </label>
                </div>
              </div>

              <div v-if="formError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ formError }}</div>
              <p class="modal-hint">{{ t('proxy.editor.hint') }}</p>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="editorOpen = false">{{ t('proxy.editor.cancel') }}</button>
              <button type="submit" class="button button--primary" :disabled="saving">
                <span v-if="saving" class="spinner" /><Icon v-else name="play" :size="15" />{{ t('proxy.editor.save') }}
              </button>
            </footer>
          </form>
        </div>
      </Transition>
    </Teleport>
  </div>
</template>
