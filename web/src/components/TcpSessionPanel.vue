<script setup lang="ts">
import { computed, nextTick, onActivated, onBeforeUnmount, onDeactivated, onMounted, ref, watch } from 'vue'
import Icon from './Icon.vue'
import { del, errorMessage, get, post } from '../api'
import { useI18nStore } from '../stores/i18n'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'TcpSessionPanel' })

interface TcpSessionInfo {
  id: string
  host: string
  port: number
  state: 'connecting' | 'connected' | 'closing' | 'closed' | 'error'
  error?: string
  closeReason?: string
  createdAt?: string
  connectedAt?: string
  closedAt?: string
  sentBytes?: number
  receivedBytes?: number
  nextSeq?: number
  queuedWrites?: number
  queuedBytes?: number
}

interface TcpPayload {
  length: number
  truncated?: boolean
  encoding: 'utf8' | 'base64'
  text?: string
  base64?: string
  hex?: string
}

interface TcpEvent {
  seq: number
  at: string
  type: string
  payload?: TcpPayload
  error?: string
  reason?: string
  fin?: boolean
}

const i18n = useI18nStore()
const t = i18n.t
const ui = useUiStore()

const host = ref('127.0.0.1')
const port = ref(9000)
const connectTimeoutMs = ref(8000)
const creating = ref(false)
const sessions = ref<TcpSessionInfo[]>([])
const selectedId = ref('')
const events = ref<TcpEvent[]>([])
const lastSeq = ref(0)
const sendPayload = ref('')
const sendEncoding = ref<'text' | 'hex'>('text')
const sending = ref(false)
const eventLog = ref<HTMLElement | null>(null)
let pollTimer: number | undefined
let pollInFlight = false
let pollAgain = false

const selected = computed(() => sessions.value.find((session) => session.id === selectedId.value) || null)
const canSend = computed(() => selected.value?.state === 'connected' && !sending.value)

function stateLabel(state: string) {
  return t(`transport.session.state.${state}`)
}

function eventLabel(type: string) {
  return t(`transport.session.event.${type}`)
}

function eventText(event: TcpEvent) {
  if (event.type === 'error') return event.error || ''
  if (event.type === 'closed') return event.reason || ''
  if (event.type === 'peer-closed') {
    return event.fin ? t('transport.session.finClean') : t('transport.session.finReset')
  }
  const payload = event.payload
  if (!payload) return ''
  return payload.encoding === 'base64' ? payload.hex || payload.base64 || '' : payload.text || ''
}

function formatBytes(bytes = 0) {
  if (!bytes) return '0 B'
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`
}

function formatTime(value?: string) {
  if (!value) return '—'
  return new Date(value).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

async function loadSessions() {
  try {
    const response = await get<{ ok: boolean; sessions: TcpSessionInfo[] }>('/api/tcp/sessions')
    if (response.ok) sessions.value = response.sessions
    if (selectedId.value && !sessions.value.some((session) => session.id === selectedId.value)) {
      selectedId.value = ''
    }
  } catch {
    // The list is supplementary; keep the panel usable if it fails once.
  }
}

async function createSession() {
  if (!host.value.trim() || !port.value) {
    ui.notify('warning', t('transport.session.notify.hostRequired'))
    return
  }
  creating.value = true
  try {
    const response = await post<{ ok: boolean; session: TcpSessionInfo; error?: string }>(
      '/api/tcp/sessions',
      {
        host: host.value.trim(),
        port: port.value,
        connectTimeoutMs: connectTimeoutMs.value,
      },
    )
    if (!response.ok || !response.session) {
      ui.notify('error', t('transport.session.notify.createFailed'), response.error)
      return
    }
    await loadSessions()
    selectSession(response.session.id)
    ui.notify('success', t('transport.session.notify.created'), `${host.value}:${port.value}`)
  } catch (err) {
    ui.toastError(err, t('transport.session.notify.createFailed'))
  } finally {
    creating.value = false
  }
}

function selectSession(id: string) {
  if (selectedId.value === id) return
  selectedId.value = id
  events.value = []
  lastSeq.value = 0
  void pollSession(true)
}

async function send() {
  const session = selected.value
  if (!session || !sendPayload.value || !canSend.value) return
  sending.value = true
  try {
    const response = await post<{ ok: boolean; error?: string }>(
      `/api/tcp/sessions/${session.id}/send`,
      { payload: sendPayload.value, encoding: sendEncoding.value },
    )
    if (!response.ok) {
      ui.notify('error', t('transport.session.notify.sendFailed'), response.error)
      return
    }
    await pollSession(true)
  } catch (err) {
    ui.toastError(err, t('transport.session.notify.sendFailed'))
  } finally {
    sending.value = false
  }
}

async function closeSession(session: TcpSessionInfo) {
  try {
    const response = await del<{ ok: boolean; error?: string }>(`/api/tcp/sessions/${session.id}`)
    if (!response.ok) {
      ui.notify('error', t('transport.session.notify.closeFailed'), response.error)
      return
    }
    await loadSessions()
    if (selectedId.value === session.id) await pollSession(true)
  } catch (err) {
    ui.toastError(err, t('transport.session.notify.closeFailed'))
  }
}

async function pollSession(force = false) {
  if (pollInFlight) {
    if (force) pollAgain = true
    return
  }
  const id = selectedId.value
  if (!id) return
  pollInFlight = true
  try {
    const response = await get<{
      ok: boolean
      session: TcpSessionInfo
      events: TcpEvent[]
      nextSeq: number
    }>(`/api/tcp/sessions/${id}/events?after=${lastSeq.value}&limit=200`)
    if (!response.ok) return
    if (response.events?.length) {
      events.value.push(...response.events)
      if (events.value.length > 1000) events.value = events.value.slice(-1000)
      lastSeq.value = response.events[response.events.length - 1].seq
      await nextTick()
      if (eventLog.value) eventLog.value.scrollTop = eventLog.value.scrollHeight
    }
    const index = sessions.value.findIndex((session) => session.id === id)
    if (index >= 0) sessions.value[index] = response.session
  } catch {
    // Polling is best-effort; the next tick retries.
  } finally {
    pollInFlight = false
    if (pollAgain) {
      pollAgain = false
      void pollSession(true)
    }
  }
}

function startPolling() {
  if (pollTimer) return
  pollTimer = window.setInterval(() => {
    if (selectedId.value) void pollSession()
  }, 900)
}

function stopPolling() {
  if (pollTimer) window.clearInterval(pollTimer)
  pollTimer = undefined
}

watch(selectedId, () => {
  if (selectedId.value) startPolling()
  else stopPolling()
})

onMounted(() => {
  void loadSessions()
  startPolling()
})
onActivated(() => {
  void loadSessions()
  startPolling()
})
onDeactivated(stopPolling)
onBeforeUnmount(stopPolling)
</script>

<template>
  <div class="session-panel">
    <section class="card session-create">
      <header class="card__head">
        <div>
          <span class="eyebrow">{{ t('transport.session.eyebrow') }}</span>
          <h3>{{ t('transport.session.title') }}</h3>
        </div>
        <span class="chip mono">{{ t('transport.session.chip') }}</span>
      </header>
      <div class="form-grid">
        <label class="field field--grow">
          <span>{{ t('transport.session.host') }}</span>
          <input v-model="host" class="mono" spellcheck="false" placeholder="127.0.0.1" />
        </label>
        <label class="field field--port">
          <span>{{ t('transport.session.port') }}</span>
          <input v-model.number="port" type="number" min="1" max="65535" class="mono" />
        </label>
        <label class="field">
          <span>{{ t('transport.session.connectTimeout') }}</span>
          <input v-model.number="connectTimeoutMs" type="number" min="500" class="mono" />
        </label>
      </div>
      <div class="form-actions">
        <span class="muted">{{ t('transport.session.hint') }}</span>
        <button class="button button--primary" :disabled="creating" @click="createSession">
          <span v-if="creating" class="spinner" />
          <Icon v-else name="bolt" :size="16" />
          {{ creating ? t('transport.session.opening') : t('transport.session.open') }}
        </button>
      </div>
    </section>

    <div class="session-grid">
      <section class="card session-list">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('transport.session.listEyebrow') }}</span><h3>{{ t('transport.session.listTitle') }}</h3></div>
          <button class="icon-button icon-button--sm" :title="t('transport.session.refresh')" @click="loadSessions">
            <Icon name="refresh" :size="15" />
          </button>
        </header>
        <div v-if="sessions.length" class="session-cards">
          <button
            v-for="session in sessions"
            :key="session.id"
            class="session-card"
            :class="{ 'session-card--active': session.id === selectedId }"
            @click="selectSession(session.id)"
          >
            <span class="session-card__dot" :class="`session-card__dot--${session.state}`" />
            <span class="session-card__body">
              <strong class="mono">{{ session.host }}:{{ session.port }}</strong>
              <small>{{ stateLabel(session.state) }} · {{ formatBytes(session.sentBytes) }} ↑ / {{ formatBytes(session.receivedBytes) }} ↓</small>
            </span>
            <span class="session-card__actions">
              <span
                v-if="session.state !== 'closed'"
                class="icon-button icon-button--sm"
                role="button"
                tabindex="0"
                :title="t('transport.session.close')"
                @click.stop="closeSession(session)"
                @keydown.enter.stop="closeSession(session)"
              >
                <Icon name="close" :size="13" />
              </span>
            </span>
          </button>
        </div>
        <div v-else class="session-empty">
          <Icon name="layers" :size="20" />
          <span>{{ t('transport.session.empty') }}</span>
        </div>
      </section>

      <section class="card session-detail">
        <header class="card__head">
          <div>
            <span class="eyebrow">{{ t('transport.session.detailEyebrow') }}</span>
            <h3>{{ selected ? `${selected.host}:${selected.port}` : t('transport.session.detailTitle') }}</h3>
          </div>
          <span v-if="selected" class="chip" :class="`chip--${selected.state === 'connected' ? 'success' : selected.state === 'error' ? 'warn' : ''}`">
            {{ stateLabel(selected.state) }}
          </span>
        </header>

        <div v-if="!selected" class="session-empty session-empty--large">
          <Icon name="layers" :size="24" />
          <span>{{ t('transport.session.selectHint') }}</span>
        </div>

        <template v-else>
          <div class="session-facts">
            <div><span>{{ t('transport.session.sent') }}</span><strong>{{ formatBytes(selected.sentBytes) }}</strong></div>
            <div><span>{{ t('transport.session.received') }}</span><strong>{{ formatBytes(selected.receivedBytes) }}</strong></div>
            <div><span>{{ t('transport.session.queued') }}</span><strong>{{ selected.queuedWrites ?? 0 }}</strong></div>
            <div><span>{{ t('transport.session.connectedAt') }}</span><strong class="mono">{{ formatTime(selected.connectedAt) }}</strong></div>
          </div>
          <div v-if="selected.error" class="inline-alert inline-alert--danger">
            <Icon name="alert" :size="16" />{{ selected.error }}
          </div>

          <div ref="eventLog" class="session-events">
            <div v-for="event in events" :key="event.seq" class="session-event" :class="`session-event--${event.type}`">
              <span class="session-event__seq mono">{{ event.seq }}</span>
              <span class="session-event__type">{{ eventLabel(event.type) }}</span>
              <span class="session-event__body mono">{{ eventText(event) }}</span>
              <span class="session-event__time mono">{{ formatTime(event.at) }}</span>
            </div>
            <div v-if="!events.length" class="session-empty">
              <span>{{ t('transport.session.noEvents') }}</span>
            </div>
          </div>

          <div class="session-send">
            <div class="session-send__row">
              <select v-model="sendEncoding" class="select">
                <option value="text">{{ t('transport.session.text') }}</option>
                <option value="hex">{{ t('transport.session.hex') }}</option>
              </select>
              <input
                v-model="sendPayload"
                class="mono"
                :placeholder="sendEncoding === 'hex' ? '48 65 6c 6c 6f' : t('transport.session.payloadPlaceholder')"
                spellcheck="false"
                :disabled="!canSend"
                @keydown.enter.exact.prevent="send"
              />
              <button class="button button--primary" :disabled="!canSend || !sendPayload" @click="send">
                <span v-if="sending" class="spinner" />
                <Icon v-else name="send" :size="15" />
                {{ t('transport.session.send') }}
              </button>
            </div>
            <span class="muted">{{ t('transport.session.sendHint') }}</span>
          </div>
        </template>
      </section>
    </div>
  </div>
</template>
