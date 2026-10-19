<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'

defineOptions({ name: 'WebSocketView' })

const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
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
  if (state.value === 'open') return t('ws.connection.connected')
  if (state.value === 'connecting') return t('ws.connection.opening')
  if (state.value === 'closing') return t('ws.connection.closing')
  return t('ws.connection.disconnected')
})

const duration = computed(() => {
  if (!connectedAt.value) return '—'
  const seconds = Math.floor((now.value - connectedAt.value.getTime()) / 1000)
  const minutes = Math.floor(seconds / 60)
  return minutes ? t('ws.duration.minutes', { minutes, seconds: seconds % 60 }) : t('ws.duration.seconds', { seconds })
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
    ui.notify('warning', t('ws.notify.urlRequired'))
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
  if (!socket.value) return
  state.value = 'closing'
  socket.value.close(1000, 'uvscope disconnect')
}

function send() {
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
        <h2>{{ t('ws.header.title') }}</h2>
        <p>{{ t('ws.header.lead') }}</p>
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
        <input v-model="protocols" class="protocol-input" :placeholder="t('ws.placeholder.protocols')" />
        <button v-if="state !== 'open' && state !== 'connecting'" class="button button--primary" @click="connect">
          <Icon name="bolt" :size="16" />{{ t('ws.action.connect') }}
        </button>
        <button v-else class="button button--danger" @click="disconnect">
          <Icon name="stop" :size="16" />{{ t('ws.action.disconnect') }}
        </button>
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
          <button class="button button--primary" :disabled="state !== 'open'" @click="send">
            <Icon name="send" :size="16" />{{ t('ws.action.send') }}
          </button>
        </div>
      </section>

      <aside class="ws-side">
        <article class="card">
          <header class="card__head"><div><span class="eyebrow">{{ t('ws.notes.eyebrow') }}</span><h3>{{ t('ws.notes.title') }}</h3></div></header>
          <div class="note-list">
            <div class="note-row"><span>{{ t('ws.notes.origin') }}</span><strong class="mono">{{ origin }}</strong></div>
            <div class="note-row"><span>{{ t('ws.notes.mixedContent') }}</span><strong>{{ t('ws.notes.mixedContentBefore') }} <span class="mono">ws://</span></strong></div>
            <div class="note-row"><span>{{ t('ws.notes.certificate') }}</span><strong>{{ t('ws.notes.certificateBefore') }} <span class="mono">wss://</span> {{ t('ws.notes.certificateAfter') }}</strong></div>
            <div class="note-row"><span>{{ t('ws.notes.persistence') }}</span><strong>{{ t('ws.notes.persistenceBody') }}</strong></div>
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
