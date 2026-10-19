<script setup lang="ts">
import { computed, onActivated, onBeforeUnmount, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { del, errorMessage, get, post } from '../api'
import type { HistoryEntry, HttpHeader, HttpResult } from '../types'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'HttpView' })

const ui = useUiStore()
const method = ref('GET')
const url = ref('https://httpbin.org/anything')
const protocol = ref<'http1' | 'http2' | 'http3'>('http1')
const headers = ref<HttpHeader[]>([
  { name: 'accept', value: 'application/json' },
  { name: 'user-agent', value: 'uvscope/0.1.0' },
])
const body = ref('')
const bodyType = ref('application/json')
const timeoutMs = ref(20000)
const insecure = ref(false)
const compression = ref(false)
const sending = ref(false)
const result = ref<HttpResult | null>(null)
const responseTab = ref<'body' | 'headers' | 'timing'>('body')
const requestTab = ref<'headers' | 'body'>('headers')
const advancedOpen = ref(false)
const history = ref<HistoryEntry[]>([])
const historyLoading = ref(false)
const startedAt = ref<number>(0)
const elapsed = ref(0)
let ticker: number | undefined

const methods = ['GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'HEAD', 'OPTIONS', 'TRACE']

const statusTone = computed(() => {
  const status = result.value?.status ?? 0
  if (!result.value?.ok) return 'bad'
  if (status >= 500) return 'bad'
  if (status >= 400) return 'warn'
  if (status >= 300) return 'redirect'
  return 'ok'
})

const responseBody = computed(() => {
  const res = result.value
  if (!res?.ok) return res?.error || ''
  if (res.bodyEncoding === 'base64' && res.bodyBase64) {
    try {
      const binary = atob(res.bodyBase64)
      const bytes = new Uint8Array(binary.length)
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
      return [...bytes.slice(0, 2048)]
        .map((byte, index) => byte.toString(16).padStart(2, '0') + (index % 16 === 15 ? '\n' : ' '))
        .join('')
    } catch {
      return res.bodyBase64
    }
  }
  const text = res.bodyText || ''
  const contentType = res.headers?.find((header) => header.name.toLowerCase() === 'content-type')?.value || ''
  if (contentType.includes('json') || text.trim().startsWith('{') || text.trim().startsWith('[')) {
    try {
      return JSON.stringify(JSON.parse(text), null, 2)
    } catch {
      return text
    }
  }
  return text
})

const responseIsBinary = computed(() => result.value?.bodyEncoding === 'base64')

function addHeader() {
  headers.value.push({ name: '', value: '' })
}
function removeHeader(index: number) {
  headers.value.splice(index, 1)
}
function cleanHeaders() {
  return headers.value.filter((header) => header.name.trim())
}

function downloadBody() {
  const res = result.value
  if (!res) return
  let blob: Blob
  if (res.bodyEncoding === 'base64' && res.bodyBase64) {
    const binary = atob(res.bodyBase64)
    const bytes = new Uint8Array(binary.length)
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
    blob = new Blob([bytes])
  } else {
    blob = new Blob([res.bodyText || ''], { type: 'text/plain' })
  }
  const anchor = document.createElement('a')
  anchor.href = URL.createObjectURL(blob)
  anchor.download = `uvscope-response-${Date.now()}.bin`
  anchor.click()
  window.setTimeout(() => URL.revokeObjectURL(anchor.href), 1000)
}

async function copy(text: string) {
  try {
    await navigator.clipboard.writeText(text)
    ui.notify('success', 'Copied to clipboard')
  } catch {
    ui.notify('error', 'Clipboard access was denied')
  }
}

async function send() {
  if (!url.value.trim()) {
    ui.notify('warning', 'A URL is required')
    return
  }
  if (sending.value) return
  sending.value = true
  result.value = null
  startedAt.value = performance.now()
  elapsed.value = 0
  ticker = window.setInterval(() => {
    elapsed.value = Math.round(performance.now() - startedAt.value)
  }, 100)
  try {
    const outgoing = cleanHeaders()
    if (body.value && !outgoing.some((header) => header.name.toLowerCase() === 'content-type')) {
      outgoing.push({ name: 'content-type', value: bodyType.value })
    }
    const payload = {
      url: url.value.trim(),
      method: method.value,
      protocol: protocol.value,
      headers: outgoing,
      body: body.value,
      timeoutMs: timeoutMs.value,
      insecure: insecure.value,
      compression: compression.value,
    }
    result.value = await post<HttpResult>('/api/http/send', payload)
    responseTab.value = 'body'
    await loadHistory()
    if (!result.value.ok) ui.notify('error', 'Request failed', result.value.error)
  } catch (err) {
    result.value = { ok: false, error: errorMessage(err) }
    ui.toastError(err, 'Request failed')
  } finally {
    sending.value = false
    if (ticker) window.clearInterval(ticker)
    elapsed.value = Math.round(performance.now() - startedAt.value)
  }
}

async function loadHistory() {
  historyLoading.value = true
  try {
    const response = await get<{ entries: HistoryEntry[] }>('/api/history?kind=http&limit=30')
    history.value = response.entries
  } catch {
    // History is supplementary; the workbench stays usable if it fails.
  } finally {
    historyLoading.value = false
  }
}

function restore(entry: HistoryEntry) {
  const request = (entry.detail?.request ?? {}) as Record<string, unknown>
  if (typeof request.url === 'string') url.value = request.url
  if (typeof request.method === 'string') method.value = request.method
  const preference = request.protocolPreference
  if (preference === 'http2' || preference === 'http3' || preference === 'http1') protocol.value = preference
}

async function clearHistory() {
  if (!window.confirm('Clear HTTP request history for all users?')) return
  try {
    await del('/api/history?kind=http')
    history.value = []
    ui.notify('success', 'HTTP history cleared')
  } catch (err) {
    ui.toastError(err)
  }
}

function formatBytes(bytes: number) {
  if (!bytes) return '0 B'
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`
}

function formatTime(value: string) {
  return new Date(value).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

function onKeydown(event: KeyboardEvent) {
  if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
    event.preventDefault()
    void send()
  }
}

onMounted(() => {
  window.addEventListener('keydown', onKeydown)
  void loadHistory()
})
onActivated(() => void loadHistory())
onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKeydown)
  if (ticker) window.clearInterval(ticker)
})
</script>

<template>
  <div class="page http-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">HTTP Studio</span>
        <h2>Requests without the mystery.</h2>
        <p>Compose any method, inspect raw headers, timing and bytes. Sent by the local libuvcpp daemon.</p>
      </div>
      <div class="page-head__actions">
        <span class="chip mono"><Icon name="terminal" :size="13" /> Ctrl + Enter</span>
      </div>
    </header>

    <div class="workbench">
      <div class="workbench__main">
        <section class="request-card">
          <div class="request-line">
            <div class="method-select" :class="`method-select--${method.toLowerCase()}`">
              <select v-model="method" aria-label="HTTP method">
                <option v-for="item in methods" :key="item" :value="item">{{ item }}</option>
              </select>
              <Icon name="chevron-down" :size="14" />
            </div>
            <input
              v-model="url"
              class="url-input"
              spellcheck="false"
              placeholder="https://api.example.com/v1/resource"
              @keydown.enter.exact.prevent="send"
            />
            <button class="button button--primary send-button" :disabled="sending" @click="send">
              <span v-if="sending" class="spinner" />
              <Icon v-else name="send" :size="16" />
              {{ sending ? 'Sending' : 'Send' }}
            </button>
          </div>

          <div class="request-toolbar">
            <div class="protocol-switch" role="group" aria-label="Protocol preference">
              <button :class="{ active: protocol === 'http1' }" @click="protocol = 'http1'">HTTP/1.1</button>
              <button :class="{ active: protocol === 'http2' }" title="HTTP/2 preference — experimental in 0.1.0" @click="protocol = 'http2'">H2 <em>exp</em></button>
              <button :class="{ active: protocol === 'http3' }" title="HTTP/3 preference — experimental in 0.1.0" @click="protocol = 'http3'">H3 <em>exp</em></button>
            </div>
            <div class="request-toolbar__right">
              <button class="ghost-toggle" :class="{ active: advancedOpen }" @click="advancedOpen = !advancedOpen">
                <Icon name="settings" :size="15" /> Options
              </button>
            </div>
          </div>

          <Transition name="reveal">
            <div v-if="advancedOpen" class="advanced-row">
              <label><span>Timeout (ms)</span><input v-model.number="timeoutMs" type="number" min="1000" max="120000" /></label>
              <label class="switch-field"><span>Allow insecure TLS</span><button class="switch" :class="{ on: insecure }" @click="insecure = !insecure"><i /></button></label>
              <label class="switch-field"><span>Request compression</span><button class="switch" :class="{ on: compression }" @click="compression = !compression"><i /></button></label>
            </div>
          </Transition>

          <div class="sub-tabs">
            <button :class="{ active: requestTab === 'headers' }" @click="requestTab = 'headers'">
              Headers <span class="count">{{ cleanHeaders().length }}</span>
            </button>
            <button :class="{ active: requestTab === 'body' }" @click="requestTab = 'body'">Body</button>
          </div>

          <div v-show="requestTab === 'headers'" class="editor-pane">
            <div class="kv-head"><span>Header</span><span>Value</span><span /></div>
            <div v-for="(header, index) in headers" :key="index" class="kv-row">
              <input v-model="header.name" placeholder="header-name" spellcheck="false" class="mono" />
              <input v-model="header.value" placeholder="value" spellcheck="false" class="mono" />
              <button class="icon-button icon-button--sm" title="Remove header" @click="removeHeader(index)">
                <Icon name="close" :size="14" />
              </button>
            </div>
            <button class="add-row" @click="addHeader"><Icon name="plus" :size="14" />Add header</button>
          </div>

          <div v-show="requestTab === 'body'" class="editor-pane">
            <div class="body-toolbar">
              <select v-model="bodyType" class="select">
                <option value="application/json">JSON</option>
                <option value="application/xml">XML</option>
                <option value="text/plain">Text</option>
                <option value="application/x-www-form-urlencoded">Form URL encoded</option>
                <option value="application/octet-stream">Binary / raw</option>
              </select>
              <span class="muted mono">{{ body.length }} chars</span>
            </div>
            <textarea v-model="body" class="code-input" spellcheck="false" placeholder="Request body…" />
          </div>
        </section>

        <section class="response-card">
          <header class="response-head">
            <div class="response-head__title">
              <span class="eyebrow">Response</span>
              <div v-if="result" class="response-meta">
                <span class="status-pill" :class="`status-pill--${statusTone}`">
                  {{ result.ok ? result.status : 'ERR' }}
                </span>
                <span class="mono">{{ result.ok ? result.statusText : result.error }}</span>
              </div>
              <span v-else class="muted">Waiting for a request</span>
            </div>
            <div class="response-head__actions">
              <template v-if="result?.ok">
                <span class="meta-pill mono">{{ result.httpVersion }}</span>
                <span class="meta-pill mono">{{ result.timing?.totalMs ?? 0 }} ms</span>
                <span class="meta-pill mono">{{ formatBytes(result.bodySize ?? 0) }}</span>
                <button class="icon-button icon-button--sm" title="Copy body" @click="copy(responseBody)"><Icon name="copy" :size="15" /></button>
                <button class="icon-button icon-button--sm" title="Download body" @click="downloadBody"><Icon name="download" :size="15" /></button>
              </template>
            </div>
          </header>

          <div v-if="sending" class="response-loading">
            <div class="pulse-orbit"><span /><span /><span /></div>
            <div>
              <strong>Waiting on {{ protocol.toUpperCase() }}</strong>
              <p class="mono">{{ elapsed }} ms elapsed · the daemon is still working</p>
            </div>
          </div>

          <div v-else-if="!result" class="response-empty">
            <div class="response-empty__art"><Icon name="send" :size="30" /></div>
            <h3>Ready when you are.</h3>
            <p>Send a request to see status, headers, raw body and timings. Everything stays on this machine.</p>
          </div>

          <div v-else-if="!result.ok" class="response-error">
            <div class="inline-alert inline-alert--danger">
              <Icon name="alert" :size="17" />
              <span>{{ result.error || 'Request failed' }}</span>
            </div>
            <div class="error-details mono">
              {{ result.request?.method }} {{ result.request?.url }} · {{ result.timing?.totalMs ?? 0 }} ms
            </div>
          </div>

          <template v-else>
            <div class="sub-tabs sub-tabs--response">
              <button :class="{ active: responseTab === 'body' }" @click="responseTab = 'body'">
                Body <span v-if="responseIsBinary" class="count">base64/hex</span>
              </button>
              <button :class="{ active: responseTab === 'headers' }" @click="responseTab = 'headers'">
                Headers <span class="count">{{ result.headers?.length ?? 0 }}</span>
              </button>
              <button :class="{ active: responseTab === 'timing' }" @click="responseTab = 'timing'">Timing</button>
            </div>
            <div v-if="responseTab === 'body'" class="response-body">
              <pre class="code-output">{{ responseBody }}</pre>
              <div v-if="result.bodyTruncated" class="truncation-note">
                <Icon name="alert" :size="14" />Preview truncated at 4 MiB.
              </div>
            </div>
            <div v-else-if="responseTab === 'headers'" class="response-headers">
              <div v-for="header in result.headers" :key="header.name + header.value" class="header-line">
                <span class="header-name mono">{{ header.name }}</span>
                <span class="header-value mono">{{ header.value }}</span>
              </div>
            </div>
            <div v-else class="timing-panel">
              <div class="timing-row">
                <div class="timing-row__label"><Icon name="globe" :size="15" />Connect</div>
                <div class="timing-bar"><span :style="{ width: `${Math.min(100, ((result.timing?.connectMs ?? 0) / Math.max(1, result.timing?.totalMs ?? 1)) * 100)}%` }" /></div>
                <span class="mono">{{ result.timing?.connectMs ?? 0 }} ms</span>
              </div>
              <div class="timing-row">
                <div class="timing-row__label"><Icon name="bolt" :size="15" />Total</div>
                <div class="timing-bar timing-bar--total"><span style="width: 100%" /></div>
                <span class="mono">{{ result.timing?.totalMs ?? 0 }} ms</span>
              </div>
              <div class="timing-facts">
                <div><span>Protocol</span><strong>{{ result.httpVersion || '—' }}</strong></div>
                <div><span>ALPN</span><strong class="mono">{{ result.alpn || '—' }}</strong></div>
                <div><span>Body bytes</span><strong>{{ result.bodySize ?? 0 }}</strong></div>
                <div><span>Sent at</span><strong class="mono">{{ result.startedAt }}</strong></div>
              </div>
            </div>
          </template>
        </section>
      </div>

      <aside class="history-panel">
        <header class="history-head">
          <div><span class="eyebrow">Recent</span><h3>Request history</h3></div>
          <button class="icon-button icon-button--sm" title="Refresh history" @click="loadHistory"><Icon name="refresh" :size="15" /></button>
        </header>
        <div v-if="historyLoading" class="history-empty"><span class="spinner" />Loading…</div>
        <div v-else-if="history.length" class="history-list">
          <button v-for="entry in history" :key="entry.id" class="history-item" @click="restore(entry)">
            <span class="status-code" :class="entry.status === 'success' ? 'status-code--ok' : 'status-code--bad'">{{ entry.httpStatus || 'ERR' }}</span>
            <div class="history-item__body">
              <strong class="mono">{{ entry.summary }}</strong>
              <span>{{ formatTime(entry.createdAt) }} · {{ entry.latencyMs }} ms</span>
            </div>
          </button>
        </div>
        <div v-else class="history-empty">
          <Icon name="clock" :size="18" />
          <span>No requests sent yet.</span>
        </div>
        <button v-if="history.length" class="history-clear" @click="clearHistory">
          <Icon name="trash" :size="14" />Clear history
        </button>
      </aside>
    </div>
  </div>
</template>
