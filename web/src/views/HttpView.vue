<script setup lang="ts">
import { computed, onActivated, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import Icon from '../components/Icon.vue'
import Socks5ProxyFields from '../components/Socks5ProxyFields.vue'
import { del, errorMessage, get, post } from '../api'
import type { HistoryEntry, HttpDownload, HttpHeader, HttpResult } from '../types'
import { useUiStore } from '../stores/ui'
import { useAuthStore } from '../stores/auth'
import { useI18nStore } from '../stores/i18n'
import {
  base64ToBytes,
  buildHtmlDocument,
  bytesToText,
  contentTypeOf,
  previewKindOf,
  type PreviewKind,
} from '../lib/preview'

defineOptions({ name: 'HttpView' })

const ui = useUiStore()
const auth = useAuthStore()
const i18n = useI18nStore()
const t = i18n.t
const method = ref('GET')
const url = ref('https://httpbin.org/anything')
const protocol = ref<'http1' | 'http2' | 'http3'>('http1')
const headers = ref<HttpHeader[]>([
  { name: 'accept', value: 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8' },
  {
    name: 'user-agent',
    value:
      `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) ` +
      `Chrome/131.0.0.0 Safari/537.36 uvscope/${auth.version}`,
  },
])
const body = ref('')
const bodyType = ref('application/json')
const timeoutMs = ref(20000)
const insecure = ref(false)
const decompress = ref(true)
const bodyMode = ref<'raw' | 'urlencoded' | 'multipart'>('raw')
const urlEncodedFields = ref<Array<{ name: string; value: string }>>([{ name: '', value: '' }])
const multipartFields = ref<Array<{ name: string; value: string; kind: 'text' | 'file'; file: File | null }>>([
  { name: '', value: '', kind: 'text', file: null },
])
const capabilities = ref({ http2: true, http3: false, quic: false, brotli: true })
const downloading = ref(false)
const downloadState = ref<HttpDownload | null>(null)
let downloadTimer: number | undefined
const proxy = ref({ enabled: false, host: '127.0.0.1', port: 1080, username: '', password: '' })
const sending = ref(false)
const result = ref<HttpResult | null>(null)
const responseTab = ref<'body' | 'preview' | 'headers' | 'timing'>('body')
const allowRemotePreview = ref(false)
const allowScriptsPreview = ref(false)
const previewUrl = ref('')
const previewDocUrl = ref('')
const previewDocId = ref('')
let previewDocSeq = 0
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

const contentType = computed(() => contentTypeOf(result.value?.headers))
const previewKind = computed<PreviewKind>(() => previewKindOf(contentType.value))
const previewAvailable = computed(
  () => result.value?.ok === true && previewKind.value !== 'none',
)

const responseBytes = computed<Uint8Array | null>(() => {
  const res = result.value
  if (!res?.ok) return null
  if (res.bodyEncoding === 'base64' && res.bodyBase64) {
    try {
      return base64ToBytes(res.bodyBase64)
    } catch {
      return null
    }
  }
  return new TextEncoder().encode(res.bodyText || '')
})

const htmlSource = computed(() => {
  if (previewKind.value !== 'html') return ''
  const bytes = responseBytes.value
  return bytes ? bytesToText(bytes) : ''
})

const htmlPreviewDocument = computed(() => {
  if (previewKind.value !== 'html') return ''
  return buildHtmlDocument(htmlSource.value, {
    allowRemote: allowRemotePreview.value,
    baseUrl: result.value?.request?.url,
  })
})

function revokePreviewUrl() {
  if (!previewUrl.value) return
  URL.revokeObjectURL(previewUrl.value)
  previewUrl.value = ''
}

function revokeScriptPreview() {
  previewDocSeq += 1
  if (previewDocId.value) {
    void del(`/api/preview/${previewDocId.value}`).catch(() => {
      // The token expires on its own; cleanup is best-effort.
    })
    previewDocId.value = ''
  }
  previewDocUrl.value = ''
}

async function loadServerPreview() {
  revokeScriptPreview()
  const needsServer = allowScriptsPreview.value || allowRemotePreview.value
  if (!needsServer || previewKind.value !== 'html' ||
      responseTab.value !== 'preview' || !htmlSource.value) {
    return
  }
  const seq = ++previewDocSeq
  try {
    const response = await post<{ ok: boolean; id?: string; url?: string; error?: string }>(
      '/api/preview',
      {
        html: htmlSource.value,
        baseUrl: result.value?.request?.url,
        mode: allowScriptsPreview.value ? 'scripts' : 'safe',
      },
    )
    if (seq !== previewDocSeq) {
      if (response.id) void del(`/api/preview/${response.id}`).catch(() => {})
      return
    }
    if (!response.ok || !response.url || !response.id) {
      ui.notify('error', t('http.preview.scriptsFailed'), response.error)
      return
    }
    previewDocId.value = response.id
    previewDocUrl.value = response.url
  } catch (err) {
    ui.toastError(err, t('http.preview.scriptsFailed'))
  }
}

function toggleScripts(event: Event) {
  const input = event.target as HTMLInputElement
  if (input.checked && !window.confirm(t('http.preview.scriptsWarning'))) {
    input.checked = false
    return
  }
  allowScriptsPreview.value = input.checked
  if (input.checked) allowRemotePreview.value = true
}

watch(
  [htmlSource, allowScriptsPreview, allowRemotePreview, responseTab],
  () => void loadServerPreview(),
)

watch(
  [() => result.value, previewKind, responseTab],
  () => {
    revokePreviewUrl()
    if (responseTab.value !== 'preview') return
    const kind = previewKind.value
    if (kind === 'none' || kind === 'html') return
    const bytes = responseBytes.value
    if (!bytes) return
    const type = contentType.value.split(';', 1)[0].trim() || 'application/octet-stream'
    const copy = new Uint8Array(bytes)
    previewUrl.value = URL.createObjectURL(new Blob([copy], { type }))
  },
  { immediate: true },
)

watch(previewKind, (kind) => {
  if (kind === 'none' && responseTab.value === 'preview') responseTab.value = 'body'
})

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
  // The probe clips at 4 MiB; ask the daemon to re-fetch and stream the full
  // response to disk instead of handing the browser a truncated blob.
  if (res.bodyTruncated) {
    void startStreamDownload()
    return
  }
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
    ui.notify('success', t('http.notify.copied'))
  } catch {
    ui.notify('error', t('http.notify.clipboardDenied'))
  }
}

function addUrlEncodedField() {
  urlEncodedFields.value.push({ name: '', value: '' })
}

function removeUrlEncodedField(index: number) {
  urlEncodedFields.value.splice(index, 1)
}

function addMultipartField() {
  multipartFields.value.push({ name: '', value: '', kind: 'text', file: null })
}

function removeMultipartField(index: number) {
  multipartFields.value.splice(index, 1)
}

function pickMultipartFile(field: { file: File | null }, event: Event) {
  field.file = (event.target as HTMLInputElement).files?.[0] ?? null
}

function multipartName(value: string): string {
  return value.replace(/["\r\n]/g, '_')
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = ''
  const chunk = 0x8000
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk))
  }
  return btoa(binary)
}

async function buildPayload() {
  const outgoing = cleanHeaders()
  let requestBody = body.value
  let bodyEncoding = 'text'
  let derivedContentType = ''
  if (bodyMode.value === 'urlencoded') {
    const params = new URLSearchParams()
    for (const field of urlEncodedFields.value) {
      if (field.name.trim()) params.append(field.name.trim(), field.value)
    }
    requestBody = params.toString()
    derivedContentType = 'application/x-www-form-urlencoded'
  } else if (bodyMode.value === 'multipart') {
    const boundary = `----uvscope${Date.now().toString(16)}${Math.random().toString(16).slice(2)}`
    const parts: BlobPart[] = []
    for (const field of multipartFields.value) {
      const name = field.name.trim()
      if (!name) continue
      if (field.kind === 'file' && field.file) {
        parts.push(
          `--${boundary}\r\nContent-Disposition: form-data; name="${multipartName(name)}"; ` +
            `filename="${multipartName(field.file.name)}"\r\n` +
            `Content-Type: ${field.file.type || 'application/octet-stream'}\r\n\r\n`,
        )
        parts.push(field.file)
        parts.push('\r\n')
      } else if (field.kind === 'text') {
        parts.push(
          `--${boundary}\r\nContent-Disposition: form-data; name="${multipartName(name)}"\r\n\r\n` +
            `${field.value}\r\n`,
        )
      }
    }
    parts.push(`--${boundary}--\r\n`)
    const blob = new Blob(parts)
    if (blob.size > 8 * 1024 * 1024) throw new Error(t('http.body.tooLarge'))
    requestBody = bytesToBase64(new Uint8Array(await blob.arrayBuffer()))
    bodyEncoding = 'base64'
    derivedContentType = `multipart/form-data; boundary=${boundary}`
  }
  const contentTypeIndex = outgoing.findIndex(
    (header) => header.name.toLowerCase() === 'content-type',
  )
  if (derivedContentType) {
    if (contentTypeIndex >= 0) outgoing.splice(contentTypeIndex, 1)
    outgoing.push({ name: 'content-type', value: derivedContentType })
  } else if (requestBody && contentTypeIndex < 0) {
    outgoing.push({ name: 'content-type', value: bodyType.value })
  }
  return {
    url: url.value.trim(),
    method: method.value,
    protocol: protocol.value,
    headers: outgoing,
    body: requestBody,
    bodyEncoding,
    timeoutMs: timeoutMs.value,
    insecure: insecure.value,
    decompress: decompress.value,
    proxy: proxy.value.enabled
      ? {
          host: proxy.value.host.trim(),
          port: proxy.value.port,
          username: proxy.value.username,
          password: proxy.value.password,
        }
      : undefined,
  }
}

async function send() {
  if (!url.value.trim()) {
    ui.notify('warning', t('http.notify.urlRequired'))
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
    result.value = await post<HttpResult>('/api/http/send', await buildPayload())
    responseTab.value = 'body'
    await loadHistory()
    if (!result.value.ok) ui.notify('error', t('http.requestFailed'), result.value.error)
  } catch (err) {
    result.value = { ok: false, error: errorMessage(err) }
    ui.toastError(err, t('http.requestFailed'))
  } finally {
    sending.value = false
    if (ticker) window.clearInterval(ticker)
    elapsed.value = Math.round(performance.now() - startedAt.value)
  }
}

function stopDownloadPolling() {
  if (downloadTimer) window.clearInterval(downloadTimer)
  downloadTimer = undefined
}

function triggerBrowserDownload(url: string) {
  const anchor = document.createElement('a')
  anchor.href = url
  anchor.download = ''
  document.body.appendChild(anchor)
  anchor.click()
  anchor.remove()
}

function pollDownload(id: string) {
  stopDownloadPolling()
  downloadTimer = window.setInterval(async () => {
    try {
      const response = await get<{ ok: boolean; download?: HttpDownload }>(
        `/api/http/download/${id}`,
      )
      const state = response.download
      if (!state) return
      downloadState.value = state
      if (state.state === 'ready') {
        stopDownloadPolling()
        downloading.value = false
        triggerBrowserDownload(state.url)
        ui.notify('success', t('http.download.ready'), state.filename)
      } else if (state.state === 'error' || state.state === 'cancelled') {
        stopDownloadPolling()
        downloading.value = false
        if (state.state === 'error') {
          ui.notify('error', t('http.download.failed'), state.error)
        }
      }
    } catch {
      // Keep polling; the daemon may be busy or restarting.
    }
  }, 400)
}

async function startStreamDownload() {
  if (downloading.value || !url.value.trim()) return
  downloading.value = true
  downloadState.value = {
    id: '',
    state: 'running',
    url: '',
    bytes: 0,
    totalBytes: -1,
  }
  try {
    const response = await post<{ ok: boolean; error?: string; download?: HttpDownload }>(
      '/api/http/download',
      await buildPayload(),
    )
    if (!response.ok || !response.download?.id) {
      throw new Error(response.error || t('http.download.failed'))
    }
    downloadState.value = response.download
    pollDownload(response.download.id)
  } catch (err) {
    downloading.value = false
    downloadState.value = null
    ui.toastError(err, t('http.download.failed'))
  }
}

async function cancelDownload() {
  const id = downloadState.value?.id
  stopDownloadPolling()
  downloading.value = false
  downloadState.value = null
  if (!id) return
  await del(`/api/http/download/${id}`).catch(() => {
    // The job expires on its own; cancellation is best-effort.
  })
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
  if (!window.confirm(t('http.history.clearConfirm'))) return
  try {
    await del('/api/history?kind=http')
    history.value = []
    ui.notify('success', t('http.history.cleared'))
  } catch (err) {
    ui.toastError(err, t('http.somethingWentWrong'))
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

async function loadCapabilities() {
  try {
    const health = await get<{ capabilities?: typeof capabilities.value }>('/api/health')
    if (health.capabilities) capabilities.value = health.capabilities
  } catch {
    // Keep the conservative defaults; the API still reports unsupported builds.
  }
}

onMounted(() => {
  window.addEventListener('keydown', onKeydown)
  void loadHistory()
  void loadCapabilities()
})
onActivated(() => void loadHistory())
onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKeydown)
  if (ticker) window.clearInterval(ticker)
  stopDownloadPolling()
  revokePreviewUrl()
  revokeScriptPreview()
})
</script>

<template>
  <div class="page http-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('http.eyebrow') }}</span>
        <h2>{{ t('http.title') }}</h2>
        <p>{{ t('http.lead') }}</p>
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
              <select v-model="method" :aria-label="t('http.methodAria')">
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
              {{ sending ? t('http.sending') : t('http.send') }}
            </button>
          </div>

          <div class="request-toolbar">
            <div class="protocol-switch" role="group" :aria-label="t('http.protocol.group')">
              <button :class="{ active: protocol === 'http1' }" @click="protocol = 'http1'">HTTP/1.1</button>
              <button
                :class="{ active: protocol === 'http2' }"
                :disabled="!capabilities.http2"
                :title="capabilities.http2 ? t('http.protocol.http2Title') : t('http.protocol.http2Unavailable')"
                @click="protocol = 'http2'"
              >H2</button>
              <button
                :class="{ active: protocol === 'http3' }"
                :disabled="!capabilities.http3"
                :title="capabilities.http3 ? t('http.protocol.http3Title') : t('http.protocol.http3Unavailable')"
                @click="protocol = 'http3'"
              >H3 <em>{{ t('http.protocol.experimental') }}</em></button>
            </div>
            <div class="request-toolbar__right">
              <button class="ghost-toggle" :class="{ active: advancedOpen }" @click="advancedOpen = !advancedOpen">
                <Icon name="settings" :size="15" /> {{ t('http.options') }}
              </button>
            </div>
          </div>

          <Transition name="reveal">
            <div v-if="advancedOpen" class="advanced-row">
              <label><span>{{ t('http.timeout') }}</span><input v-model.number="timeoutMs" type="number" min="1000" max="120000" /></label>
              <label class="switch-field"><span>{{ t('http.allowInsecureTls') }}</span><button class="switch" :class="{ on: insecure }" @click="insecure = !insecure"><i /></button></label>
              <label class="switch-field"><span>{{ t('http.requestCompression') }}</span><button class="switch" :class="{ on: decompress }" @click="decompress = !decompress"><i /></button></label>
            </div>
          </Transition>
          <Socks5ProxyFields v-if="advancedOpen" v-model="proxy" />

          <div class="sub-tabs">
            <button :class="{ active: requestTab === 'headers' }" @click="requestTab = 'headers'">
              {{ t('http.request.tab.headers') }} <span class="count">{{ cleanHeaders().length }}</span>
            </button>
            <button :class="{ active: requestTab === 'body' }" @click="requestTab = 'body'">{{ t('http.request.tab.body') }}</button>
          </div>

          <div v-show="requestTab === 'headers'" class="editor-pane">
            <div class="kv-head"><span>{{ t('http.headers.name') }}</span><span>{{ t('http.headers.value') }}</span><span /></div>
            <div v-for="(header, index) in headers" :key="index" class="kv-row">
              <input v-model="header.name" :placeholder="t('http.headers.namePlaceholder')" spellcheck="false" class="mono" />
              <input v-model="header.value" :placeholder="t('http.headers.valuePlaceholder')" spellcheck="false" class="mono" />
              <button class="icon-button icon-button--sm" :title="t('http.headers.remove')" @click="removeHeader(index)">
                <Icon name="close" :size="14" />
              </button>
            </div>
            <button class="add-row" @click="addHeader"><Icon name="plus" :size="14" />{{ t('http.headers.add') }}</button>
          </div>

          <div v-show="requestTab === 'body'" class="editor-pane">
            <div class="body-mode">
              <button :class="{ active: bodyMode === 'raw' }" @click="bodyMode = 'raw'">{{ t('http.body.mode.raw') }}</button>
              <button :class="{ active: bodyMode === 'urlencoded' }" @click="bodyMode = 'urlencoded'">{{ t('http.body.mode.urlencoded') }}</button>
              <button :class="{ active: bodyMode === 'multipart' }" @click="bodyMode = 'multipart'">{{ t('http.body.mode.multipart') }}</button>
            </div>

            <template v-if="bodyMode === 'raw'">
              <div class="body-toolbar">
                <select v-model="bodyType" class="select">
                  <option value="application/json">JSON</option>
                  <option value="application/xml">XML</option>
                  <option value="text/plain">{{ t('http.body.typeText') }}</option>
                  <option value="application/x-www-form-urlencoded">{{ t('http.body.typeForm') }}</option>
                  <option value="application/octet-stream">{{ t('http.body.typeBinary') }}</option>
                </select>
                <span class="muted mono">{{ t('http.body.chars', { count: body.length }) }}</span>
              </div>
              <textarea v-model="body" class="code-input" spellcheck="false" :placeholder="t('http.body.placeholder')" />
            </template>

            <template v-else-if="bodyMode === 'urlencoded'">
              <div class="kv-head"><span>{{ t('http.body.fieldName') }}</span><span>{{ t('http.body.fieldValue') }}</span><span /></div>
              <div v-for="(field, index) in urlEncodedFields" :key="index" class="kv-row">
                <input v-model="field.name" :placeholder="t('http.body.fieldNamePlaceholder')" spellcheck="false" class="mono" />
                <input v-model="field.value" :placeholder="t('http.body.fieldValuePlaceholder')" spellcheck="false" class="mono" />
                <button class="icon-button icon-button--sm" :title="t('http.body.removeField')" @click="removeUrlEncodedField(index)"><Icon name="close" :size="14" /></button>
              </div>
              <button class="add-row" @click="addUrlEncodedField"><Icon name="plus" :size="14" />{{ t('http.body.addField') }}</button>
              <p class="muted body-hint">{{ t('http.body.urlencodedHint') }}</p>
            </template>

            <template v-else>
              <div class="multipart-row multipart-row--head">
                <span>{{ t('http.body.fieldType') }}</span>
                <span>{{ t('http.body.fieldName') }}</span>
                <span>{{ t('http.body.fieldValue') }}</span>
                <span />
              </div>
              <div v-for="(field, index) in multipartFields" :key="index" class="multipart-row">
                <select v-model="field.kind" class="select select--compact">
                  <option value="text">{{ t('http.body.fieldText') }}</option>
                  <option value="file">{{ t('http.body.fieldFile') }}</option>
                </select>
                <input v-model="field.name" :placeholder="t('http.body.fieldNamePlaceholder')" spellcheck="false" class="mono" />
                <input v-if="field.kind === 'text'" v-model="field.value" :placeholder="t('http.body.fieldValuePlaceholder')" spellcheck="false" class="mono" />
                <input v-else type="file" class="mono" @change="pickMultipartFile(field, $event)" />
                <button class="icon-button icon-button--sm" :title="t('http.body.removeField')" @click="removeMultipartField(index)"><Icon name="close" :size="14" /></button>
              </div>
              <button class="add-row" @click="addMultipartField"><Icon name="plus" :size="14" />{{ t('http.body.addField') }}</button>
              <p class="muted body-hint">{{ t('http.body.multipartHint') }}</p>
            </template>
          </div>
        </section>

        <section class="response-card">
          <header class="response-head">
            <div class="response-head__title">
              <span class="eyebrow">{{ t('http.response.eyebrow') }}</span>
              <div v-if="result" class="response-meta">
                <span class="status-pill" :class="`status-pill--${statusTone}`">
                  {{ result.ok ? result.status : 'ERR' }}
                </span>
                <span class="mono">{{ result.ok ? result.statusText : result.error }}</span>
              </div>
              <span v-else class="muted">{{ t('http.response.waiting') }}</span>
            </div>
            <div class="response-head__actions">
              <template v-if="result?.ok">
                <span class="meta-pill mono">{{ result.httpVersion }}</span>
                <span class="meta-pill mono">{{ result.timing?.totalMs ?? 0 }} ms</span>
                <span class="meta-pill mono">{{ formatBytes(result.bodySize ?? 0) }}</span>
                <button class="icon-button icon-button--sm" :title="t('http.response.copyBody')" @click="copy(responseBody)"><Icon name="copy" :size="15" /></button>
                <button class="icon-button icon-button--sm" :title="result.bodyTruncated ? t('http.download.streamTitle') : t('http.response.downloadBody')" @click="downloadBody"><Icon name="download" :size="15" /></button>
                <span v-if="downloading || downloadState" class="meta-pill mono">
                  {{ downloadState?.totalBytes && downloadState.totalBytes > 0
                    ? t('http.download.progressTotal', { bytes: formatBytes(downloadState.bytes), total: formatBytes(downloadState.totalBytes) })
                    : t('http.download.progress', { bytes: formatBytes(downloadState?.bytes ?? 0) }) }}
                </span>
                <button v-if="downloading" class="icon-button icon-button--sm" :title="t('http.download.cancel')" @click="cancelDownload"><Icon name="close" :size="14" /></button>
              </template>
            </div>
          </header>

          <div v-if="sending" class="response-loading">
            <div class="pulse-orbit"><span /><span /><span /></div>
            <div>
              <strong>{{ t('http.response.waitingOn', { protocol: protocol.toUpperCase() }) }}</strong>
              <p class="mono">{{ t('http.response.elapsed', { elapsed }) }}</p>
            </div>
          </div>

          <div v-else-if="!result" class="response-empty">
            <div class="response-empty__art"><Icon name="send" :size="30" /></div>
            <h3>{{ t('http.response.readyTitle') }}</h3>
            <p>{{ t('http.response.readyLead') }}</p>
          </div>

          <div v-else-if="!result.ok" class="response-error">
            <div class="inline-alert inline-alert--danger">
              <Icon name="alert" :size="17" />
              <span>{{ result.error || t('http.requestFailed') }}</span>
            </div>
            <div class="error-details mono">
              {{ result.request?.method }} {{ result.request?.url }} · {{ result.timing?.totalMs ?? 0 }} ms
            </div>
          </div>

          <template v-else>
            <div class="sub-tabs sub-tabs--response">
              <button :class="{ active: responseTab === 'body' }" @click="responseTab = 'body'">
                {{ t('http.response.tab.body') }} <span v-if="responseIsBinary" class="count">base64/hex</span>
              </button>
              <button
                v-if="previewAvailable"
                :class="{ active: responseTab === 'preview' }"
                @click="responseTab = 'preview'"
              >
                {{ t('http.response.tab.preview') }} <span class="count">{{ previewKind }}</span>
              </button>
              <button :class="{ active: responseTab === 'headers' }" @click="responseTab = 'headers'">
                {{ t('http.response.tab.headers') }} <span class="count">{{ result.headers?.length ?? 0 }}</span>
              </button>
              <button :class="{ active: responseTab === 'timing' }" @click="responseTab = 'timing'">{{ t('http.response.tab.timing') }}</button>
            </div>
            <div v-if="responseTab === 'body'" class="response-body">
              <pre class="code-output">{{ responseBody }}</pre>
              <div v-if="result.bodyTruncated" class="truncation-note">
                <Icon name="alert" :size="14" />{{ t('http.response.truncated') }}
              </div>
            </div>
            <div v-else-if="responseTab === 'preview'" class="response-preview">
              <div class="preview-toolbar">
                <span class="chip mono">{{ contentType || t('http.response.unknownContentType') }}</span>
                <span class="muted">{{ formatBytes(result.bodySize ?? 0) }}</span>
                <label v-if="previewKind === 'html'" class="preview-toggle">
                  <input v-model="allowRemotePreview" type="checkbox" :disabled="allowScriptsPreview" />
                  {{ t('http.preview.loadRemote') }}
                </label>
                <label v-if="previewKind === 'html'" class="preview-toggle preview-toggle--unsafe">
                  <input type="checkbox" :checked="allowScriptsPreview" @change="toggleScripts" />
                  {{ t('http.preview.runScripts') }}
                </label>
                <span class="muted preview-hint">
                  <template v-if="previewKind !== 'html'">{{ t('http.preview.localBrowser') }}</template>
                  <template v-else-if="allowScriptsPreview">{{ t('http.preview.scriptsEnabled') }}</template>
                  <template v-else>{{ t('http.preview.scriptsDisabled') }}</template>
                </span>
              </div>
              <div v-if="previewKind === 'html' && !allowRemotePreview && !allowScriptsPreview" class="preview-warning">
                <Icon name="alert" :size="14" />
                {{ t('http.preview.remoteHint') }}
              </div>
              <div v-if="allowScriptsPreview" class="preview-warning">
                <Icon name="alert" :size="14" />
                {{ t('http.preview.scriptsHint') }}
              </div>
              <div v-if="result.bodyTruncated" class="preview-warning">
                <Icon name="alert" :size="14" />
                {{ t('http.preview.warning') }}
              </div>
              <iframe
                v-if="previewKind === 'html' && allowScriptsPreview && previewDocUrl"
                class="preview-frame preview-frame--html"
                sandbox="allow-scripts"
                :src="previewDocUrl"
                referrerpolicy="no-referrer"
                :title="t('http.preview.htmlTitle')"
              />
              <div v-else-if="previewKind === 'html' && allowScriptsPreview" class="preview-empty">
                {{ t('http.preview.scriptsPreparing') }}
              </div>
              <iframe
                v-else-if="previewKind === 'html' && allowRemotePreview && previewDocUrl"
                class="preview-frame preview-frame--html"
                sandbox=""
                :src="previewDocUrl"
                referrerpolicy="no-referrer"
                :title="t('http.preview.htmlTitle')"
              />
              <div v-else-if="previewKind === 'html' && allowRemotePreview" class="preview-empty">
                {{ t('http.preview.scriptsPreparing') }}
              </div>
              <iframe
                v-else-if="previewKind === 'html'"
                class="preview-frame preview-frame--html"
                sandbox=""
                :srcdoc="htmlPreviewDocument"
                :title="t('http.preview.htmlTitle')"
              />
              <div v-else-if="previewKind === 'image'" class="preview-media">
                <img :src="previewUrl" :alt="t('http.preview.imageAlt')" />
              </div>
              <audio v-else-if="previewKind === 'audio'" class="preview-audio" :src="previewUrl" controls />
              <video v-else-if="previewKind === 'video'" class="preview-video" :src="previewUrl" controls />
              <iframe
                v-else-if="previewKind === 'pdf'"
                class="preview-frame"
                :src="previewUrl"
                :title="t('http.preview.pdfTitle')"
              />
              <div v-else class="preview-empty">{{ t('http.preview.empty') }}</div>
            </div>
            <div v-else-if="responseTab === 'headers'" class="response-headers">
              <div v-for="header in result.headers" :key="header.name + header.value" class="header-line">
                <span class="header-name mono">{{ header.name }}</span>
                <span class="header-value mono">{{ header.value }}</span>
              </div>
            </div>
            <div v-else class="timing-panel">
              <div class="timing-row">
                <div class="timing-row__label"><Icon name="globe" :size="15" />{{ t('http.timing.connect') }}</div>
                <div class="timing-bar"><span :style="{ width: `${Math.min(100, ((result.timing?.connectMs ?? 0) / Math.max(1, result.timing?.totalMs ?? 1)) * 100)}%` }" /></div>
                <span class="mono">{{ result.timing?.connectMs ?? 0 }} ms</span>
              </div>
              <div class="timing-row">
                <div class="timing-row__label"><Icon name="bolt" :size="15" />{{ t('http.timing.total') }}</div>
                <div class="timing-bar timing-bar--total"><span style="width: 100%" /></div>
                <span class="mono">{{ result.timing?.totalMs ?? 0 }} ms</span>
              </div>
              <div class="timing-facts">
                <div><span>{{ t('http.timing.protocol') }}</span><strong>{{ result.httpVersion || '—' }}</strong></div>
                <div><span>ALPN</span><strong class="mono">{{ result.alpn || '—' }}</strong></div>
                <div><span>{{ t('http.timing.bodyBytes') }}</span><strong>{{ result.bodySize ?? 0 }}</strong></div>
                <div v-if="result.compression?.contentEncoding">
                  <span>{{ t('http.compression.encoding') }}</span>
                  <strong class="mono">{{ result.compression.contentEncoding }}{{ result.compression.decoded ? ' ✓' : '' }}</strong>
                </div>
                <div v-if="result.compression?.wireBodySize !== undefined">
                  <span>{{ t('http.compression.wire') }}</span>
                  <strong class="mono">
                    {{ formatBytes(result.compression.wireBodySize) }} → {{ formatBytes(result.compression.bodySize) }}
                  </strong>
                </div>
                <div v-if="result.compression?.error">
                  <span>{{ t('http.compression.failed') }}</span>
                  <strong class="mono">{{ result.compression.error }}</strong>
                </div>
                <div><span>{{ t('http.timing.streamId') }}</span><strong class="mono">{{ result.streamId ?? 0 }}</strong></div>
                <div><span>{{ t('http.timing.retryable') }}</span><strong>{{ result.retryable ? t('http.timing.yes') : t('http.timing.no') }}</strong></div>
                <div v-if="result.goaway">
                  <span>{{ t('http.timing.goaway') }}</span>
                  <strong class="mono">
                    {{ result.goaway.received ? `${result.goaway.errorCode} / ${result.goaway.lastStreamId}` : t('http.timing.no') }}
                  </strong>
                </div>
                <div><span>{{ t('http.timing.sentAt') }}</span><strong class="mono">{{ result.startedAt }}</strong></div>
              </div>
              <div v-if="result.h2" class="h2-frames">
                <header class="h2-frames__head">
                  <span>{{ t('http.h2.title') }}</span>
                  <span class="mono">{{ t('http.h2.count', { count: result.h2.frameCount }) }}</span>
                </header>
                <div v-if="result.h2.frames.length" class="h2-frames__list">
                  <div v-for="(frame, index) in result.h2.frames" :key="index" class="h2-frame">
                    <span class="h2-frame__type">{{ frame.type }}</span>
                    <span class="mono" :title="t('http.h2.stream')">{{ frame.streamId }}</span>
                    <span class="mono" :title="t('http.h2.length')">{{ frame.length }} B</span>
                    <span class="mono" :title="t('http.h2.flags')">
                      {{ frame.flags ? `0x${frame.flags.toString(16)}` : '—' }}
                    </span>
                    <span v-if="frame.errorCode" class="h2-frame__error mono">{{ frame.errorCode }}</span>
                  </div>
                </div>
                <p v-else class="h2-frames__empty">{{ t('http.h2.empty') }}</p>
                <p v-if="result.h2.framesTruncated" class="h2-frames__hint">
                  {{ t('http.h2.truncated', { limit: result.h2.maxFrames }) }}
                </p>
              </div>
            </div>
          </template>
        </section>
      </div>

      <aside class="history-panel">
        <header class="history-head">
          <div><span class="eyebrow">{{ t('http.history.eyebrow') }}</span><h3>{{ t('http.history.title') }}</h3></div>
          <button class="icon-button icon-button--sm" :title="t('http.history.refresh')" @click="loadHistory"><Icon name="refresh" :size="15" /></button>
        </header>
        <div v-if="historyLoading" class="history-empty"><span class="spinner" />{{ t('http.history.loading') }}</div>
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
          <span>{{ t('http.history.empty') }}</span>
        </div>
        <button v-if="history.length && auth.isAdmin" class="history-clear" @click="clearHistory">
          <Icon name="trash" :size="14" />{{ t('http.history.clear') }}
        </button>
      </aside>
    </div>
  </div>
</template>
