<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { del, get, post } from '../api'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'
import type { HttpHeader, WsEvent, WsSession } from '../types'

defineOptions({ name: 'WebSocketView' })

const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
const mode = ref<'backend' | 'browser'>('backend')
const url = ref('wss://echo.websocket.events')
const protocols = ref('')
const wsHeaders = ref<HttpHeader[]>([{ name: '', value: '' }])
const insecure = ref(false)
const socket = ref<WebSocket | null>(null)
const state = ref<'closed' | 'connecting' | 'open' | 'closing' | 'error'>('closed')
const message = ref('')
const sendMode = ref<'text' | 'hex'>('text')
const log = ref<Array<{ id: number; direction: 'in' | 'out' | 'system'; kind: 'text' | 'binary' | 'event'; data: string; at: Date; bytes: number }>>([])
const logEl = ref<HTMLElement | null>(null)
const sent = ref(0)
const received = ref(0)
const connectedAt = ref<Date | null>(null)
const now = ref(Date.now())
const backend = ref<WsSession | null>(null)
const sessionId = ref('')
let counter = 1
let clock: number | undefined
let pollTimer: number | undefined
let lastSeq = 0

const connectionLabel = computed(() => {
  if (state.value === 'open') return t('ws.connection.connected')
  if (state.value === 'connecting') return t('ws.connection.opening')
  if (state.value === 'closing') return t('ws.connection.closing')
  if (state.value === 'error') return t('ws.connection.error')
  return t('ws.connection.disconnected')
})

const duration = computed(() => {
  if (!connectedAt.value) return '—'
  const seconds = Math.max(0, Math.floor((now.value - connectedAt.value.getTime()) / 1000))
  const minutes = Math.floor(seconds / 60)
  return minutes ? t('ws.duration.minutes', { minutes, seconds: seconds % 60 }) : t('ws.duration.seconds', { seconds })
})

const byteCount = computed(() => {
  if (mode.value === 'backend' && backend.value) {
    return backend.value.sentBytes + backend.value.receivedBytes
  }
  return log.value.reduce((total, item) => total + item.bytes, 0)
})
const origin = window.location.origin

function append(direction: 'in' | 'out' | 'system', data: string, kind: 'text' | 'binary' | 'event' = 'text') {
  log.value.push({ id: counter++, direction, kind, data, at: new Date(), bytes: new Blob([data]).size })
  if (log.value.length > 600) log.value.splice(0, log.value.length - 600)
  void nextTick(() => {
    if (logEl.value) logEl.value.scrollTop = logEl.value.scrollHeight
  })
}

function hexOfBase64(value: string): string {
  try {
    const binary = atob(value)
    return [...binary].map((c) => c.charCodeAt(0).toString(16).padStart(2, '0')).join(' ')
  } catch {
    return value
  }
}

function stopPolling() {
  if (pollTimer) window.clearInterval(pollTimer)
  pollTimer = undefined
}

function applyBackendEvent(event: WsEvent) {
  switch (event.type) {
    case 'connecting':
      append('system', t('ws.log.connecting', { url: url.value.trim() }), 'event')
      break
    case 'open':
      state.value = 'open'
      connectedAt.value = new Date()
      now.value = Date.now()
      append('system', t('ws.log.handshake'), 'event')
      ui.notify('success', t('ws.notify.connected'))
      break
    case 'text':
      received.value += 1
      append('in', event.text ?? '', 'text')
      break
    case 'binary':
      received.value += 1
      append('in', hexOfBase64(event.base64 ?? ''), 'binary')
      break
    case 'frame':
      if (event.opcode === 'PING' || event.opcode === 'PONG' || event.opcode === 'CLOSE') {
        const size = event.length ?? 0
        let detail = `FRAME ${event.opcode} · ${size} B`
        if (event.opcode === 'CLOSE' && event.closeCode !== undefined) {
          detail += ` · ${event.closeCode}${event.closeReason ? ` ${event.closeReason}` : ''}`
        }
        append('system', detail, 'event')
      }
      break
    case 'closing':
      append('system', t('ws.log.closing', { code: event.code ?? 1000 }), 'event')
      break
    case 'close':
      state.value = 'closed'
      connectedAt.value = null
      stopPolling()
      append('system', `${t('ws.log.closed', { code: event.code ?? 1000 })}${event.reason ? t('ws.log.closeReason', { reason: event.reason }) : ''}`, 'event')
      break
    case 'error':
      state.value = 'error'
      append('system', event.error || t('ws.log.transportError'), 'event')
      break
    case 'sent':
      break
    default:
      break
  }
}

async function pollBackend() {
  if (!sessionId.value) return
  try {
    const response = await get<{ ok: boolean; events: WsEvent[]; session?: WsSession }>(
      `/api/ws/sessions/${sessionId.value}/events?after=${lastSeq}&limit=200`,
    )
    if (response.session) backend.value = response.session
    for (const event of response.events ?? []) {
      lastSeq = Math.max(lastSeq, event.seq)
      applyBackendEvent(event)
    }
    if (backend.value?.state === 'error' && state.value !== 'closed') state.value = 'error'
  } catch {
    // The session may have been deleted elsewhere; keep polling for the close event.
  }
}

function startPolling() {
  stopPolling()
  pollTimer = window.setInterval(() => void pollBackend(), 350)
}

async function connectBackend() {
  state.value = 'connecting'
  try {
    const response = await post<{ ok: boolean; session?: WsSession; error?: string }>(
      '/api/ws/sessions',
      {
        url: url.value.trim(),
        headers: wsHeaders.value.filter((header) => header.name.trim()),
        subprotocol: protocols.value.trim(),
        insecure: insecure.value,
        timeoutMs: 15000,
      },
    )
    if (!response.ok || !response.session?.id) {
      throw new Error(response.error || t('ws.notify.errorBody'))
    }
    sessionId.value = response.session.id
    backend.value = response.session
    lastSeq = 0
    startPolling()
  } catch (err) {
    state.value = 'error'
    const text = err instanceof Error ? err.message : String(err)
    append('system', t('ws.log.openFailed', { message: text }), 'event')
    ui.notify('error', t('ws.notify.openFailedTitle'), text)
  }
}

function connect() {
  if (socket.value || sessionId.value) return
  if (!url.value.trim()) {
    ui.notify('warning', t('ws.notify.urlRequired'))
    return
  }
  if (mode.value === 'backend') {
    void connectBackend()
    return
  }
  state.value = 'connecting'
  try {
    const ws = protocols.value.trim() ? new WebSocket(url.value.trim(), protocols.value.split(',').map((p) => p.trim()).filter(Boolean)) : new WebSocket(url.value.trim())
    ws.binaryType = 'arraybuffer'
    socket.value = ws
    append('system', t('ws.log.connecting', { url: url.value.trim() }))
    ws.onopen = () => {
      state.value = 'open'
      connectedAt.value = new Date()
      now.value = Date.now()
      clock = window.setInterval(() => (now.value = Date.now()), 1000)
      append('system', `${t('ws.log.handshake')}${ws.protocol ? t('ws.log.subprotocol', { protocol: ws.protocol }) : ''}`)
      ui.notify('success', t('ws.notify.connected'))
    }
    ws.onmessage = async (event) => {
      received.value++
      if (typeof event.data === 'string') {
        let text = event.data
        try {
          text = JSON.stringify(JSON.parse(event.data), null, 2)
        } catch {
          // Non-JSON text is rendered verbatim.
        }
        append('in', text, 'text')
      } else if (event.data instanceof ArrayBuffer) {
        const bytes = new Uint8Array(event.data)
        append('in', [...bytes].map((b) => b.toString(16).padStart(2, '0')).join(' '), 'binary')
      }
    }
    ws.onerror = () => {
      append('system', t('ws.log.transportError'))
      ui.notify('error', t('ws.notify.errorTitle'), t('ws.notify.errorBody'))
    }
    ws.onclose = (event) => {
      state.value = 'closed'
      connectedAt.value = null
      socket.value = null
      if (clock) window.clearInterval(clock)
      append('system', `${t('ws.log.closed', { code: event.code })}${event.reason ? t('ws.log.closeReason', { reason: event.reason }) : ''}`)
    }
  } catch (err) {
    state.value = 'closed'
    const text = err instanceof Error ? err.message : String(err)
    append('system', t('ws.log.openFailed', { message: text }))
    ui.notify('error', t('ws.notify.openFailedTitle'), text)
  }
}

function disconnect() {
  if (mode.value === 'backend') {
    if (!sessionId.value) return
    state.value = 'closing'
    void post(`/api/ws/sessions/${sessionId.value}/close`, { code: 1000, reason: 'uvscope disconnect' }).catch(() => {
      // The close event from the poller settles the UI.
    })
    return
  }
  if (!socket.value) return
  state.value = 'closing'
  socket.value.close(1000, 'uvscope disconnect')
}

async function deleteBackend() {
  if (!sessionId.value) return
  const id = sessionId.value
  stopPolling()
  sessionId.value = ''
  backend.value = null
  state.value = 'closed'
  connectedAt.value = null
  try {
    await del(`/api/ws/sessions/${id}`)
    append('system', t('ws.log.deleted'), 'event')
    ui.notify('success', t('ws.notify.deleted'))
  } catch (err) {
    ui.toastError(err, t('ws.notify.deleteFailed'))
  }
}

function switchMode(next: 'backend' | 'browser') {
  if (mode.value === next) return
  if (socket.value) {
    socket.value.close(1001, 'mode switched')
    socket.value = null
  }
  if (sessionId.value) void deleteBackend()
  mode.value = next
  state.value = 'closed'
  connectedAt.value = null
}

function addHeader() {
  wsHeaders.value.push({ name: '', value: '' })
}

function removeHeader(index: number) {
  wsHeaders.value.splice(index, 1)
}

async function send() {
  if (mode.value === 'backend') {
    if (!sessionId.value || state.value !== 'open') {
      ui.notify('warning', t('ws.notify.connectFirst'))
      return
    }
    let payload = message.value
    let encoding: 'text' | 'hex' = 'text'
    let type: 'text' | 'binary' = 'text'
    if (sendMode.value === 'hex') {
      const text = message.value.replace(/[^0-9a-fA-F]/g, '')
      if (text.length % 2 !== 0) {
        ui.notify('warning', t('ws.notify.hexEven'))
        return
      }
      payload = text
      encoding = 'hex'
      type = 'binary'
    }
    try {
      const response = await post<{ ok: boolean; error?: string }>(
        `/api/ws/sessions/${sessionId.value}/send`,
        { type, encoding, payload },
      )
      if (!response.ok) throw new Error(response.error || t('ws.notify.errorBody'))
      sent.value += 1
      append('out', sendMode.value === 'hex' ? payload.replace(/(..)/g, '$1 ').trim() : payload, sendMode.value === 'hex' ? 'binary' : 'text')
      message.value = ''
    } catch (err) {
      ui.toastError(err, t('ws.notify.errorTitle'))
    }
    return
  }
  if (!socket.value || state.value !== 'open') {
    ui.notify('warning', t('ws.notify.connectFirst'))
    return
  }
  if (sendMode.value === 'hex') {
    const text = message.value.replace(/[^0-9a-fA-F]/g, '')
    if (text.length % 2 !== 0) {
      ui.notify('warning', t('ws.notify.hexEven'))
      return
    }
    const bytes = new Uint8Array(text.length / 2)
    for (let i = 0; i < bytes.length; i++) bytes[i] = Number.parseInt(text.slice(i * 2, i * 2 + 2), 16)
    socket.value.send(bytes)
    sent.value++
    append('out', [...bytes].map((b) => b.toString(16).padStart(2, '0')).join(' '), 'binary')
  } else {
    socket.value.send(message.value)
    sent.value++
    append('out', message.value, 'text')
  }
  message.value = ''
}

async function sendPing() {
  if (mode.value !== 'backend' || !sessionId.value || state.value !== 'open') {
    ui.notify('warning', t('ws.notify.connectFirst'))
    return
  }
  try {
    const payload = message.value || 'ping'
    await post(`/api/ws/sessions/${sessionId.value}/send`, { type: 'ping', encoding: 'text', payload })
    append('out', `PING · ${payload}`, 'event')
    message.value = ''
  } catch (err) {
    ui.toastError(err, t('ws.notify.errorTitle'))
  }
}

function clearLog() {
  log.value = []
  sent.value = 0
  received.value = 0
}

function formatTime(at: Date) {
  return at.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit', fractionalSecondDigits: 3 } as Intl.DateTimeFormatOptions)
}

onBeforeUnmount(() => {
  if (clock) window.clearInterval(clock)
  stopPolling()
  socket.value?.close(1001, 'view disposed')
  socket.value = null
  if (sessionId.value) {
    void del(`/api/ws/sessions/${sessionId.value}`).catch(() => {})
  }
})
</script>

<template>
  <div class="page ws-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">WebSocket</span>
        <h2>{{ t('ws.header.title') }}</h2>
        <p>{{ t('ws.header.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <span class="chip" :class="state === 'open' ? 'chip--success' : state === 'connecting' || state === 'closing' ? 'chip--warn' : ''">
          <i class="status-dot" :class="{ 'status-dot--idle': state !== 'open' }" />{{ connectionLabel }}
        </span>
      </div>
    </header>

    <section class="ws-connect card">
      <div class="ws-mode-switch" role="group" :aria-label="t('ws.mode.group')">
        <button :class="{ active: mode === 'backend' }" @click="switchMode('backend')">{{ t('ws.mode.backend') }}</button>
        <button :class="{ active: mode === 'browser' }" @click="switchMode('browser')">{{ t('ws.mode.browser') }}</button>
      </div>
      <div class="ws-url-line">
        <div class="protocol-badge mono">WS</div>
        <input v-model="url" class="url-input" spellcheck="false" placeholder="wss://example.com/socket" @keydown.enter.prevent="state === 'open' ? disconnect() : connect()" />
        <input v-model="protocols" class="protocol-input" :placeholder="t('ws.placeholder.protocols')" />
        <button v-if="state !== 'open' && state !== 'connecting'" class="button button--primary" @click="connect">
          <Icon name="bolt" :size="16" />{{ t('ws.action.connect') }}
        </button>
        <button v-else class="button button--danger" @click="disconnect">
          <Icon name="stop" :size="16" />{{ t('ws.action.disconnect') }}
        </button>
      </div>
      <div v-if="mode === 'backend'" class="ws-backend-options">
        <div class="ws-headers">
          <div class="kv-head"><span>{{ t('ws.headers.name') }}</span><span>{{ t('ws.headers.value') }}</span><span /></div>
          <div v-for="(header, index) in wsHeaders" :key="index" class="kv-row">
            <input v-model="header.name" spellcheck="false" class="mono" placeholder="Authorization" :disabled="state === 'open' || state === 'connecting'" />
            <input v-model="header.value" spellcheck="false" class="mono" placeholder="Bearer …" :disabled="state === 'open' || state === 'connecting'" />
            <button class="icon-button icon-button--sm" :title="t('ws.headers.remove')" :disabled="state === 'open' || state === 'connecting'" @click="removeHeader(index)"><Icon name="close" :size="14" /></button>
          </div>
          <button class="add-row" :disabled="state === 'open' || state === 'connecting'" @click="addHeader"><Icon name="plus" :size="14" />{{ t('ws.headers.add') }}</button>
        </div>
        <label class="switch-field">
          <span>{{ t('ws.options.insecure') }}</span>
          <button class="switch" :class="{ on: insecure }" :disabled="state === 'open' || state === 'connecting'" @click="insecure = !insecure"><i /></button>
        </label>
        <button v-if="sessionId" class="ghost-toggle" @click="deleteBackend"><Icon name="trash" :size="14" />{{ t('ws.action.delete') }}</button>
      </div>
      <div class="ws-stats">
        <div><span>{{ t('ws.stats.state') }}</span><strong>{{ connectionLabel }}</strong></div>
        <div><span>{{ t('ws.stats.session') }}</span><strong>{{ duration }}</strong></div>
        <div><span>{{ t('ws.stats.sent') }}</span><strong>{{ t('ws.stats.frames', { count: sent }) }}</strong></div>
        <div><span>{{ t('ws.stats.received') }}</span><strong>{{ t('ws.stats.frames', { count: received }) }}</strong></div>
        <div><span>{{ t('ws.stats.traffic') }}</span><strong>{{ byteCount }} B</strong></div>
      </div>
    </section>

    <div class="ws-workspace">
      <section class="card ws-console">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('ws.logCard.eyebrow') }}</span><h3>{{ t('ws.logCard.title') }}</h3></div>
          <div class="card__tools">
            <button class="ghost-toggle" @click="clearLog"><Icon name="trash" :size="14" />{{ t('ws.logCard.clear') }}</button>
          </div>
        </header>
        <div ref="logEl" class="ws-log">
          <div v-if="!log.length" class="ws-log__empty">
            <div class="response-empty__art"><Icon name="pulse" :size="26" /></div>
            <strong>{{ t('ws.empty.title') }}</strong>
            <span>{{ t('ws.empty.body') }}</span>
          </div>
          <div v-for="entry in log" :key="entry.id" class="ws-frame" :class="`ws-frame--${entry.direction}`">
            <div class="ws-frame__gutter">
              <span class="ws-frame__dir">{{ entry.direction === 'in' ? 'IN' : entry.direction === 'out' ? 'OUT' : 'SYS' }}</span>
              <span class="mono">{{ formatTime(entry.at) }}</span>
            </div>
            <div class="ws-frame__body">
              <div class="ws-frame__meta">
                <span>{{ entry.kind }}</span>
                <span class="mono">{{ entry.bytes }} B</span>
              </div>
              <pre>{{ entry.data }}</pre>
            </div>
          </div>
        </div>
        <div class="ws-compose">
          <div class="ws-compose__mode">
            <button :class="{ active: sendMode === 'text' }" @click="sendMode = 'text'">{{ t('ws.mode.text') }}</button>
            <button :class="{ active: sendMode === 'hex' }" @click="sendMode = 'hex'">Hex</button>
          </div>
          <textarea
            v-model="message"
            class="ws-compose__input"
            :placeholder="sendMode === 'hex' ? '48 65 6c 6c 6f' : t('ws.placeholder.message')"
            spellcheck="false"
            @keydown.ctrl.enter.prevent="send"
            @keydown.meta.enter.prevent="send"
          />
          <button v-if="mode === 'backend'" class="button" :disabled="state !== 'open'" @click="sendPing">
            <Icon name="pulse" :size="16" />{{ t('ws.action.ping') }}
          </button>
          <button class="button button--primary" :disabled="state !== 'open'" @click="send">
            <Icon name="send" :size="16" />{{ t('ws.action.send') }}
          </button>
        </div>
      </section>

      <aside class="ws-side">
        <article class="card">
          <header class="card__head"><div><span class="eyebrow">{{ t('ws.notes.eyebrow') }}</span><h3>{{ t('ws.notes.title') }}</h3></div></header>
          <div v-if="mode === 'browser'" class="note-list">
            <div class="note-row"><span>{{ t('ws.notes.origin') }}</span><strong class="mono">{{ origin }}</strong></div>
            <div class="note-row"><span>{{ t('ws.notes.mixedContent') }}</span><strong>{{ t('ws.notes.mixedContentBefore') }} <span class="mono">ws://</span></strong></div>
            <div class="note-row"><span>{{ t('ws.notes.certificate') }}</span><strong>{{ t('ws.notes.certificateBefore') }} <span class="mono">wss://</span> {{ t('ws.notes.certificateAfter') }}</strong></div>
            <div class="note-row"><span>{{ t('ws.notes.persistence') }}</span><strong>{{ t('ws.notes.persistenceBody') }}</strong></div>
          </div>
          <div v-else class="note-list">
            <div class="note-row"><span>{{ t('ws.notes.backendOrigin') }}</span><strong class="mono">uvscope daemon</strong></div>
            <div class="note-row"><span>{{ t('ws.notes.backendHeaders') }}</span><strong>{{ t('ws.notes.backendHeadersBody') }}</strong></div>
            <div class="note-row"><span>{{ t('ws.notes.certificate') }}</span><strong>{{ t('ws.options.insecure') }}</strong></div>
            <div class="note-row"><span>{{ t('ws.notes.persistence') }}</span><strong>{{ t('ws.notes.backendPersistenceBody') }}</strong></div>
          </div>
        </article>
        <article class="card card--tip">
          <div class="tip-icon"><Icon name="spark" :size="20" /></div>
          <h3>{{ t('ws.tip.title') }}</h3>
          <p>{{ t('ws.tip.body') }}</p>
          <code class="mono">wss://echo.websocket.events</code>
        </article>
      </aside>
    </div>
  </div>
</template>
