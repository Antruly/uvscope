<script setup lang="ts">
import { computed, nextTick, onActivated, onBeforeUnmount, onDeactivated, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { del, errorMessage, get, post } from '../api'
import { useI18nStore } from '../stores/i18n'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'ListenersView' })

interface ListenerInfo {
  id: string
  transport: 'tcp' | 'udp'
  name?: string
  host: string
  port: number
  autoStart?: boolean
  state: 'stopped' | 'starting' | 'listening' | 'stopping' | 'error'
  error?: string
  startedAt?: string
  stoppedAt?: string
  sessionCount?: number
  activeSessions?: number
  datagrams?: number
  lastPeer?: string
  nextSeq?: number
}

interface ListenerEvent {
  seq: number
  at: string
  type: string
  peer?: string
  sessionId?: string
  payload?: { length: number; encoding: string; text?: string; hex?: string; base64?: string; truncated?: boolean }
  error?: string
  partial?: boolean
  fin?: boolean
  reason?: string
}

interface AcceptedSession {
  id: string
  listenerId: string
  peer: string
  state: string
  connectedAt?: string
  closedAt?: string
  sentBytes?: number
  receivedBytes?: number
  queuedWrites?: number
  nextSeq?: number
}

interface SessionEvent extends ListenerEvent {}

const i18n = useI18nStore()
const t = i18n.t
const ui = useUiStore()

const transport = ref<'tcp' | 'udp'>('tcp')
const name = ref('')
const host = ref('127.0.0.1')
const port = ref(9000)
const autoStart = ref(false)
const startNow = ref(true)
const creating = ref(false)

const listeners = ref<ListenerInfo[]>([])
const selectedId = ref('')
const listenerEvents = ref<ListenerEvent[]>([])
const listenerSeq = ref(0)
const sessions = ref<AcceptedSession[]>([])
const selectedSessionId = ref('')
const sessionEvents = ref<SessionEvent[]>([])
const sessionSeq = ref(0)

const payload = ref('')
const encoding = ref<'text' | 'hex'>('text')
const peerHost = ref('')
const peerPort = ref(0)
const sending = ref(false)
const eventLog = ref<HTMLElement | null>(null)
const sessionLog = ref<HTMLElement | null>(null)

let timer: number | undefined
let inFlight = false
let again = false

const selected = computed(() => listeners.value.find((item) => item.id === selectedId.value) || null)
const selectedSession = computed(
  () => sessions.value.find((item) => item.id === selectedSessionId.value) || null,
)

function stateLabel(state: string) {
  return t(`listeners.state.${state}`)
}

function eventLabel(type: string) {
  return t(`listeners.event.${type}`)
}

function sessionEventLabel(type: string) {
  return t(`listeners.sessionEvent.${type}`)
}

function sessionStateLabel(state: string) {
  return t(`listeners.sessionState.${state}`)
}

function payloadText(value?: ListenerEvent['payload']) {
  if (!value) return ''
  return value.encoding === 'base64' ? value.hex || value.base64 || '' : value.text || ''
}

function eventText(event: ListenerEvent) {
  if (event.type === 'error') return event.error || ''
  if (event.type === 'datagram') return event.payload ? `${event.peer || ''} ${payloadText(event.payload)}` : event.peer || ''
  if (event.type === 'sent') return event.payload ? `${event.peer || ''} ${payloadText(event.payload)}` : event.peer || ''
  if (event.type === 'accepted') return event.sessionId ? `${event.sessionId} ${event.peer || ''}` : event.peer || ''
  if (event.type === 'session-closed') return event.sessionId || ''
  return ''
}

function sessionEventText(event: SessionEvent) {
  if (event.type === 'error') return event.error || ''
  if (event.type === 'peer-closed') {
    return event.fin ? t('transport.session.finClean') : t('transport.session.finReset')
  }
  if (event.type === 'closed') return event.reason || ''
  return payloadText(event.payload)
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

function scrollLogs() {
  void nextTick(() => {
    if (eventLog.value) eventLog.value.scrollTop = eventLog.value.scrollHeight
    if (sessionLog.value) sessionLog.value.scrollTop = sessionLog.value.scrollHeight
  })
}

async function loadListeners() {
  try {
    const response = await get<{ ok: boolean; listeners: ListenerInfo[] }>('/api/transport/listeners')
    if (!response.ok) return
    listeners.value = response.listeners
    if (selectedId.value && !listeners.value.some((item) => item.id === selectedId.value)) {
      selectedId.value = ''
    }
  } catch {
    // Polling is best-effort.
  }
}

async function createListener() {
  if (!host.value.trim() || !port.value) {
    ui.notify('warning', t('listeners.notify.hostRequired'))
    return
  }
  creating.value = true
  try {
    const response = await post<{ ok: boolean; listener?: string; error?: string }>(
      '/api/transport/listeners',
      {
        transport: transport.value,
        name: name.value.trim(),
        host: host.value.trim(),
        port: port.value,
        autoStart: autoStart.value,
        start: startNow.value,
      },
    )
    if (!response.ok || !response.listener) {
      ui.notify('error', t('listeners.notify.createFailed'), response.error)
      return
    }
    selectListener(response.listener)
    await refreshAll(true)
    ui.notify('success', t('listeners.notify.created'), `${host.value}:${port.value}`)
  } catch (err) {
    ui.toastError(err, t('listeners.notify.createFailed'))
  } finally {
    creating.value = false
  }
}

async function startListener(listener: ListenerInfo) {
  try {
    const response = await post<{ ok: boolean; error?: string }>(
      `/api/transport/listeners/${listener.id}/start`,
    )
    if (!response.ok) ui.notify('error', t('listeners.notify.startFailed'), response.error)
    await refreshAll(true)
  } catch (err) {
    ui.toastError(err, t('listeners.notify.startFailed'))
  }
}

async function stopListener(listener: ListenerInfo) {
  try {
    const response = await post<{ ok: boolean; error?: string }>(
      `/api/transport/listeners/${listener.id}/stop`,
    )
    if (!response.ok) ui.notify('error', t('listeners.notify.stopFailed'), response.error)
    await refreshAll(true)
  } catch (err) {
    ui.toastError(err, t('listeners.notify.stopFailed'))
  }
}

async function deleteListener(listener: ListenerInfo) {
  if (!window.confirm(t('listeners.confirm.delete'))) return
  try {
    const response = await del<{ ok: boolean; error?: string }>(
      `/api/transport/listeners/${listener.id}`,
    )
    if (!response.ok) {
      ui.notify('error', t('listeners.notify.deleteFailed'), response.error)
      return
    }
    if (selectedId.value === listener.id) selectedId.value = ''
    await refreshAll(true)
    ui.notify('success', t('listeners.notify.deleted'))
  } catch (err) {
    ui.toastError(err, t('listeners.notify.deleteFailed'))
  }
}

function selectListener(id: string) {
  selectedId.value = id
  listenerEvents.value = []
  listenerSeq.value = 0
  sessions.value = []
  selectedSessionId.value = ''
  sessionEvents.value = []
  sessionSeq.value = 0
  void refreshAll(true)
}

function selectSession(id: string) {
  selectedSessionId.value = id
  sessionEvents.value = []
  sessionSeq.value = 0
  void refreshAll(true)
}

async function loadSessions(listener: ListenerInfo) {
  if (listener.transport !== 'tcp') {
    sessions.value = []
    selectedSessionId.value = ''
    return
  }
  try {
    const response = await get<{ ok: boolean; sessions: AcceptedSession[] }>(
      `/api/transport/listeners/${listener.id}/sessions`,
    )
    if (!response.ok) return
    sessions.value = response.sessions
    if (selectedSessionId.value &&
        !sessions.value.some((item) => item.id === selectedSessionId.value)) {
      selectedSessionId.value = ''
      sessionEvents.value = []
      sessionSeq.value = 0
    }
  } catch {
    // Polling is best-effort.
  }
}

async function pollListenerEvents(listener: ListenerInfo) {
  try {
    const response = await get<{
      ok: boolean
      listener: ListenerInfo
      events: ListenerEvent[]
      nextSeq: number
    }>(`/api/transport/listeners/${listener.id}/events?after=${listenerSeq.value}&limit=200`)
    if (!response.ok) return
    if (response.events?.length) {
      listenerEvents.value.push(...response.events)
      if (listenerEvents.value.length > 1000) {
        listenerEvents.value = listenerEvents.value.slice(-1000)
      }
      listenerSeq.value = response.events[response.events.length - 1].seq
      const last = [...response.events].reverse().find((event) => event.type === 'datagram')
      if (last?.peer && !peerHost.value) {
        const separator = last.peer.lastIndexOf(':')
        if (separator > 0) {
          peerHost.value = last.peer.slice(0, separator)
          peerPort.value = Number(last.peer.slice(separator + 1)) || 0
        }
      }
      scrollLogs()
    }
    const index = listeners.value.findIndex((item) => item.id === listener.id)
    if (index >= 0) listeners.value[index] = response.listener
  } catch {
    // Polling is best-effort.
  }
}

async function pollSessionEvents(listener: ListenerInfo, sessionId: string) {
  try {
    const response = await get<{
      ok: boolean
      session: AcceptedSession
      events: SessionEvent[]
      nextSeq: number
    }>(
      `/api/transport/listeners/${listener.id}/sessions/${sessionId}/events?after=${sessionSeq.value}&limit=200`,
    )
    if (!response.ok) return
    if (response.events?.length) {
      sessionEvents.value.push(...response.events)
      if (sessionEvents.value.length > 1000) {
        sessionEvents.value = sessionEvents.value.slice(-1000)
      }
      sessionSeq.value = response.events[response.events.length - 1].seq
      scrollLogs()
    }
    const index = sessions.value.findIndex((item) => item.id === sessionId)
    if (index >= 0) sessions.value[index] = response.session
  } catch {
    // Polling is best-effort.
  }
}

async function refreshAll(force = false) {
  if (inFlight) {
    if (force) again = true
    return
  }
  inFlight = true
  try {
    await loadListeners()
    const listener = selected.value
    if (!listener) return
    await pollListenerEvents(listener)
    if (listener.transport === 'tcp') {
      await loadSessions(listener)
      if (selectedSessionId.value) {
        await pollSessionEvents(listener, selectedSessionId.value)
      }
    }
  } finally {
    inFlight = false
    if (again) {
      again = false
      void refreshAll(true)
    }
  }
}

async function send() {
  const listener = selected.value
  if (!listener || !payload.value) return
  if (listener.transport === 'tcp' && !selectedSession.value) {
    ui.notify('warning', t('listeners.notify.peerRequired'))
    return
  }
  if (listener.transport === 'udp' && (!peerHost.value || !peerPort.value)) {
    ui.notify('warning', t('listeners.notify.peerRequired'))
    return
  }
  sending.value = true
  try {
    const body =
      listener.transport === 'tcp'
        ? {
            sessionId: selectedSession.value?.id,
            payload: payload.value,
            encoding: encoding.value,
          }
        : {
            peerHost: peerHost.value,
            peerPort: peerPort.value,
            payload: payload.value,
            encoding: encoding.value,
          }
    const response = await post<{ ok: boolean; error?: string }>(
      `/api/transport/listeners/${listener.id}/send`,
      body,
    )
    if (!response.ok) {
      ui.notify('error', t('listeners.notify.sendFailed'), response.error)
      return
    }
    await refreshAll(true)
  } catch (err) {
    ui.toastError(err, t('listeners.notify.sendFailed'))
  } finally {
    sending.value = false
  }
}

async function closeSession(session: AcceptedSession) {
  const listener = selected.value
  if (!listener) return
  try {
    const response = await del<{ ok: boolean; error?: string }>(
      `/api/transport/listeners/${listener.id}/sessions/${session.id}`,
    )
    if (!response.ok) {
      ui.notify('error', t('listeners.notify.stopFailed'), response.error)
      return
    }
    await refreshAll(true)
  } catch (err) {
    ui.toastError(err, t('listeners.notify.stopFailed'))
  }
}

function startPolling() {
  if (timer) return
  timer = window.setInterval(() => void refreshAll(), 900)
}

function stopPolling() {
  if (timer) window.clearInterval(timer)
  timer = undefined
}

onMounted(() => {
  void refreshAll(true)
  startPolling()
})
onActivated(() => {
  void refreshAll(true)
  startPolling()
})
onDeactivated(stopPolling)
onBeforeUnmount(stopPolling)
</script>

<template>
  <div class="page listeners-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('listeners.eyebrow') }}</span>
        <h2>{{ t('listeners.title') }}</h2>
        <p>{{ t('listeners.lead') }}</p>
      </div>
      <span class="chip mono">{{ t('listeners.create.transport') }} TCP / UDP</span>
    </header>

    <section class="card listener-create">
      <header class="card__head">
        <div><span class="eyebrow">{{ t('listeners.create.eyebrow') }}</span><h3>{{ t('listeners.create.title') }}</h3></div>
      </header>
      <div class="form-grid">
        <label class="field">
          <span>{{ t('listeners.create.transport') }}</span>
          <select v-model="transport" class="select">
            <option value="tcp">TCP</option>
            <option value="udp">UDP</option>
          </select>
        </label>
        <label class="field field--grow">
          <span>{{ t('listeners.create.name') }}</span>
          <input v-model="name" :placeholder="t('listeners.create.namePlaceholder')" spellcheck="false" />
        </label>
        <label class="field field--grow">
          <span>{{ t('listeners.create.host') }}</span>
          <input v-model="host" class="mono" spellcheck="false" />
        </label>
        <label class="field field--port">
          <span>{{ t('listeners.create.port') }}</span>
          <input v-model.number="port" type="number" min="1" max="65535" class="mono" />
        </label>
      </div>
      <div class="form-actions listener-create__actions">
        <label class="listener-check"><input v-model="autoStart" type="checkbox" />{{ t('listeners.create.autoStart') }}</label>
        <label class="listener-check"><input v-model="startNow" type="checkbox" />{{ t('listeners.create.startNow') }}</label>
        <button class="button button--primary" :disabled="creating" @click="createListener">
          <span v-if="creating" class="spinner" />
          <Icon v-else name="plus" :size="15" />
          {{ creating ? t('listeners.create.creating') : t('listeners.create.action') }}
        </button>
      </div>
    </section>

    <div class="listener-grid">
      <section class="card listener-list">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('listeners.list.eyebrow') }}</span><h3>{{ t('listeners.list.title') }}</h3></div>
          <button class="icon-button icon-button--sm" :title="t('listeners.list.refresh')" @click="refreshAll(true)">
            <Icon name="refresh" :size="15" />
          </button>
        </header>
        <div v-if="listeners.length" class="listener-cards">
          <button
            v-for="listener in listeners"
            :key="listener.id"
            class="listener-card"
            :class="{ 'listener-card--active': listener.id === selectedId }"
            @click="selectListener(listener.id)"
          >
            <span class="session-card__dot" :class="`session-card__dot--${listener.state === 'listening' ? 'connected' : listener.state}`" />
            <span class="listener-card__body">
              <strong class="mono">{{ listener.host }}:{{ listener.port }}</strong>
              <small>{{ listener.transport.toUpperCase() }} · {{ stateLabel(listener.state) }}<template v-if="listener.name"> · {{ listener.name }}</template></small>
            </span>
            <span class="chip mono">{{ listener.transport }}</span>
          </button>
        </div>
        <div v-else class="session-empty">
          <Icon name="server" :size="20" />
          <span>{{ t('listeners.list.empty') }}</span>
        </div>
      </section>

      <section class="card listener-detail">
        <header class="card__head">
          <div>
            <span class="eyebrow">{{ t('listeners.events.eyebrow') }}</span>
            <h3>{{ selected ? `${selected.host}:${selected.port}` : t('listeners.list.select') }}</h3>
          </div>
          <div v-if="selected" class="listener-actions">
            <button
              v-if="selected.state !== 'listening'"
              class="button button--sm"
              @click="startListener(selected)"
            >{{ t('listeners.action.start') }}</button>
            <button
              v-else
              class="button button--sm"
              @click="stopListener(selected)"
            >{{ t('listeners.action.stop') }}</button>
            <button class="icon-button icon-button--sm icon-button--danger" :title="t('listeners.action.delete')" @click="deleteListener(selected)">
              <Icon name="trash" :size="14" />
            </button>
          </div>
        </header>

        <div v-if="!selected" class="session-empty session-empty--large">
          <Icon name="server" :size="24" />
          <span>{{ t('listeners.list.select') }}</span>
        </div>

        <template v-else>
          <div class="listener-facts">
            <div><span>{{ t('listeners.facts.sessions') }}</span><strong>{{ selected.sessionCount ?? 0 }}</strong></div>
            <div><span>{{ t('listeners.facts.active') }}</span><strong>{{ selected.activeSessions ?? 0 }}</strong></div>
            <div><span>{{ t('listeners.facts.datagrams') }}</span><strong>{{ selected.datagrams ?? 0 }}</strong></div>
            <div><span>{{ t('listeners.facts.autoStart') }}</span><strong>{{ selected.autoStart ? t('listeners.facts.yes') : t('listeners.facts.no') }}</strong></div>
            <div><span>{{ t('listeners.facts.lastPeer') }}</span><strong class="mono">{{ selected.lastPeer || '—' }}</strong></div>
            <div><span>{{ t('listeners.facts.startedAt') }}</span><strong class="mono">{{ formatTime(selected.startedAt) }}</strong></div>
          </div>
          <div v-if="selected.error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ selected.error }}</div>

          <div ref="eventLog" class="session-events listener-events">
            <div v-for="event in listenerEvents" :key="event.seq" class="session-event" :class="`session-event--${event.type}`">
              <span class="session-event__seq mono">{{ event.seq }}</span>
              <span class="session-event__type">{{ eventLabel(event.type) }}</span>
              <span class="session-event__body mono">{{ eventText(event) }}</span>
              <span class="session-event__time mono">{{ formatTime(event.at) }}</span>
            </div>
            <div v-if="!listenerEvents.length" class="session-empty"><span>{{ t('listeners.events.empty') }}</span></div>
          </div>

          <template v-if="selected.transport === 'tcp'">
            <div class="listener-sessions">
              <div class="listener-sessions__head">
                <span class="eyebrow">{{ t('listeners.sessions.eyebrow') }}</span>
                <strong>{{ t('listeners.sessions.title') }}</strong>
              </div>
              <div v-if="sessions.length" class="listener-session-list">
                <button
                  v-for="session in sessions"
                  :key="session.id"
                  class="listener-session"
                  :class="{ 'listener-session--active': session.id === selectedSessionId }"
                  @click="selectSession(session.id)"
                >
                  <span class="session-card__dot" :class="`session-card__dot--${session.state === 'connected' ? 'connected' : session.state}`" />
                  <span class="mono">{{ session.peer }}</span>
                  <span class="muted">{{ sessionStateLabel(session.state) }} · {{ formatBytes(session.sentBytes) }} ↑ / {{ formatBytes(session.receivedBytes) }} ↓</span>
                </button>
              </div>
              <div v-else class="session-empty"><span>{{ t('listeners.sessions.empty') }}</span></div>
            </div>

            <template v-if="selectedSession">
              <div ref="sessionLog" class="session-events listener-session-events">
                <div v-for="event in sessionEvents" :key="event.seq" class="session-event" :class="`session-event--${event.type}`">
                  <span class="session-event__seq mono">{{ event.seq }}</span>
                  <span class="session-event__type">{{ sessionEventLabel(event.type) }}</span>
                  <span class="session-event__body mono">{{ sessionEventText(event) }}</span>
                  <span class="session-event__time mono">{{ formatTime(event.at) }}</span>
                </div>
              </div>
              <div class="session-send listener-send">
                <div class="session-send__row">
                  <select v-model="encoding" class="select">
                    <option value="text">{{ t('listeners.send.text') }}</option>
                    <option value="hex">{{ t('listeners.send.hex') }}</option>
                  </select>
                  <input
                    v-model="payload"
                    class="mono"
                    :placeholder="t('listeners.send.payloadPlaceholder')"
                    spellcheck="false"
                    @keydown.enter.exact.prevent="send"
                  />
                  <button class="button button--primary" :disabled="sending || !payload" @click="send">
                    <span v-if="sending" class="spinner" />
                    <Icon v-else name="send" :size="15" />
                    {{ t('listeners.send.action') }}
                  </button>
                  <button class="icon-button icon-button--sm icon-button--danger" :title="t('listeners.action.closeSession')" @click="closeSession(selectedSession)">
                    <Icon name="close" :size="14" />
                  </button>
                </div>
                <span class="muted">{{ t('listeners.send.sessionHint') }}</span>
              </div>
            </template>
          </template>

          <div v-else class="session-send listener-send">
            <div class="session-send__row">
              <input v-model="peerHost" class="mono listener-peer-host" :placeholder="t('listeners.send.peerHost')" spellcheck="false" />
              <input v-model.number="peerPort" type="number" min="1" max="65535" class="mono listener-peer-port" :placeholder="t('listeners.send.peerPort')" />
              <select v-model="encoding" class="select">
                <option value="text">{{ t('listeners.send.text') }}</option>
                <option value="hex">{{ t('listeners.send.hex') }}</option>
              </select>
              <input
                v-model="payload"
                class="mono"
                :placeholder="t('listeners.send.payloadPlaceholder')"
                spellcheck="false"
                @keydown.enter.exact.prevent="send"
              />
              <button class="button button--primary" :disabled="sending || !payload" @click="send">
                <span v-if="sending" class="spinner" />
                <Icon v-else name="send" :size="15" />
                {{ t('listeners.send.action') }}
              </button>
            </div>
            <span class="muted">{{ t('listeners.send.udpHint') }}</span>
          </div>
        </template>
      </section>
    </div>
  </div>
</template>
