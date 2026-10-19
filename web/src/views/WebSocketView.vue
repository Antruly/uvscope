<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'WebSocketView' })

const ui = useUiStore()
const url = ref('wss://echo.websocket.events')
const protocols = ref('')
const socket = ref<WebSocket | null>(null)
const state = ref<'closed' | 'connecting' | 'open' | 'closing'>('closed')
const message = ref('')
const sendMode = ref<'text' | 'hex'>('text')
const log = ref<Array<{ id: number; direction: 'in' | 'out' | 'system'; kind: 'text' | 'binary' | 'event'; data: string; at: Date; bytes: number }>>([])
const logEl = ref<HTMLElement | null>(null)
const sent = ref(0)
const received = ref(0)
const connectedAt = ref<Date | null>(null)
const now = ref(Date.now())
let counter = 1
let clock: number | undefined

const connectionLabel = computed(() => {
  if (state.value === 'open') return 'Connected'
  if (state.value === 'connecting') return 'Opening handshake'
  if (state.value === 'closing') return 'Closing'
  return 'Disconnected'
})

const duration = computed(() => {
  if (!connectedAt.value) return '—'
  const seconds = Math.floor((now.value - connectedAt.value.getTime()) / 1000)
  const minutes = Math.floor(seconds / 60)
  return minutes ? `${minutes}m ${seconds % 60}s` : `${seconds}s`
})

const byteCount = computed(() => log.value.reduce((total, item) => total + item.bytes, 0))
const origin = window.location.origin

function append(direction: 'in' | 'out' | 'system', data: string, kind: 'text' | 'binary' | 'event' = 'text') {
  log.value.push({ id: counter++, direction, kind, data, at: new Date(), bytes: new Blob([data]).size })
  if (log.value.length > 600) log.value.splice(0, log.value.length - 600)
  void nextTick(() => {
    if (logEl.value) logEl.value.scrollTop = logEl.value.scrollHeight
  })
}

function connect() {
  if (socket.value) return
  if (!url.value.trim()) {
    ui.notify('warning', 'A WebSocket URL is required')
    return
  }
  state.value = 'connecting'
  try {
    const ws = protocols.value.trim() ? new WebSocket(url.value.trim(), protocols.value.split(',').map((p) => p.trim()).filter(Boolean)) : new WebSocket(url.value.trim())
    ws.binaryType = 'arraybuffer'
    socket.value = ws
    append('system', `Connecting to ${url.value.trim()}`)
    ws.onopen = () => {
      state.value = 'open'
      connectedAt.value = new Date()
      clock = window.setInterval(() => (now.value = Date.now()), 1000)
      append('system', `Handshake complete${ws.protocol ? ` · subprotocol ${ws.protocol}` : ''}`)
      ui.notify('success', 'WebSocket connected')
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
      append('system', 'Transport error reported by the browser')
      ui.notify('error', 'WebSocket error', 'Check the URL, certificate and mixed-content rules.')
    }
    ws.onclose = (event) => {
      state.value = 'closed'
      connectedAt.value = null
      socket.value = null
      if (clock) window.clearInterval(clock)
      append('system', `Closed · code ${event.code}${event.reason ? ` · ${event.reason}` : ''}`)
    }
  } catch (err) {
    state.value = 'closed'
    const text = err instanceof Error ? err.message : String(err)
    append('system', `Could not open socket: ${text}`)
    ui.notify('error', 'Could not open WebSocket', text)
  }
}

function disconnect() {
  if (!socket.value) return
  state.value = 'closing'
  socket.value.close(1000, 'uvscope disconnect')
}

function send() {
  if (!socket.value || state.value !== 'open') {
    ui.notify('warning', 'Connect before sending a message')
    return
  }
  if (sendMode.value === 'hex') {
    const text = message.value.replace(/[^0-9a-fA-F]/g, '')
    if (text.length % 2 !== 0) {
      ui.notify('warning', 'Hex payload must have an even number of digits')
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
  socket.value?.close(1001, 'view disposed')
})
</script>

<template>
  <div class="page ws-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">WebSocket</span>
        <h2>See every frame move.</h2>
        <p>Open a real WebSocket from the browser, stream text or binary frames, and inspect each message.</p>
      </div>
      <div class="page-head__actions">
        <span class="chip" :class="state === 'open' ? 'chip--success' : state === 'connecting' ? 'chip--warn' : ''">
          <i class="status-dot" :class="{ 'status-dot--idle': state !== 'open' }" />{{ connectionLabel }}
        </span>
      </div>
    </header>

    <section class="ws-connect card">
      <div class="ws-url-line">
        <div class="protocol-badge mono">WS</div>
        <input v-model="url" class="url-input" spellcheck="false" placeholder="wss://example.com/socket" @keydown.enter.prevent="state === 'open' ? disconnect() : connect()" />
        <input v-model="protocols" class="protocol-input" placeholder="subprotocols (optional)" />
        <button v-if="state !== 'open' && state !== 'connecting'" class="button button--primary" @click="connect">
          <Icon name="bolt" :size="16" />Connect
        </button>
        <button v-else class="button button--danger" @click="disconnect">
          <Icon name="stop" :size="16" />Disconnect
        </button>
      </div>
      <div class="ws-stats">
        <div><span>State</span><strong>{{ connectionLabel }}</strong></div>
        <div><span>Session</span><strong>{{ duration }}</strong></div>
        <div><span>Sent</span><strong>{{ sent }} frames</strong></div>
        <div><span>Received</span><strong>{{ received }} frames</strong></div>
        <div><span>Traffic</span><strong>{{ byteCount }} B</strong></div>
      </div>
    </section>

    <div class="ws-workspace">
      <section class="card ws-console">
        <header class="card__head">
          <div><span class="eyebrow">Frame log</span><h3>Live trace</h3></div>
          <div class="card__tools">
            <button class="ghost-toggle" @click="clearLog"><Icon name="trash" :size="14" />Clear</button>
          </div>
        </header>
        <div ref="logEl" class="ws-log">
          <div v-if="!log.length" class="ws-log__empty">
            <div class="response-empty__art"><Icon name="pulse" :size="26" /></div>
            <strong>No frames yet</strong>
            <span>Connect and send a message to start the trace.</span>
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
            <button :class="{ active: sendMode === 'text' }" @click="sendMode = 'text'">Text</button>
            <button :class="{ active: sendMode === 'hex' }" @click="sendMode = 'hex'">Hex</button>
          </div>
          <textarea
            v-model="message"
            class="ws-compose__input"
            :placeholder="sendMode === 'hex' ? '48 65 6c 6c 6f' : 'Message payload…'"
            spellcheck="false"
            @keydown.ctrl.enter.prevent="send"
            @keydown.meta.enter.prevent="send"
          />
          <button class="button button--primary" :disabled="state !== 'open'" @click="send">
            <Icon name="send" :size="16" />Send
          </button>
        </div>
      </section>

      <aside class="ws-side">
        <article class="card">
          <header class="card__head"><div><span class="eyebrow">Browser sandbox</span><h3>Connection notes</h3></div></header>
          <div class="note-list">
            <div class="note-row"><span>Origin</span><strong class="mono">{{ origin }}</strong></div>
            <div class="note-row"><span>Mixed content</span><strong>HTTPS pages cannot open <span class="mono">ws://</span></strong></div>
            <div class="note-row"><span>Certificate</span><strong>Use a trusted <span class="mono">wss://</span> endpoint</strong></div>
            <div class="note-row"><span>Persistence</span><strong>Messages remain while this tab is cached</strong></div>
          </div>
        </article>
        <article class="card card--tip">
          <div class="tip-icon"><Icon name="spark" :size="20" /></div>
          <h3>Try a local echo</h3>
          <p>
            Point the URL at a local WebSocket echo server. uvscope keeps every frame in this tab while you
            switch between tools.
          </p>
          <code class="mono">wss://echo.websocket.events</code>
        </article>
      </aside>
    </div>
  </div>
</template>
