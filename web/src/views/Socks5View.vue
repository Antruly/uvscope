<script setup lang="ts">
import { computed, nextTick, onActivated, onBeforeUnmount, onDeactivated, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { del, get, post } from '../api'
import { useI18nStore } from '../stores/i18n'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'Socks5View' })

interface Socks5ServerInfo {
  id: string
  name?: string
  host: string
  port: number
  boundPort?: number
  autoStart?: boolean
  authRequired?: boolean
  username?: string
  state: 'stopped' | 'starting' | 'listening' | 'stopping' | 'error'
  error?: string
  startedAt?: string
  stoppedAt?: string
  activeSessions?: number
  sessionCount?: number
  totalSessions?: number
  rejectedSessions?: number
  bytesAvailable?: boolean
  isolated?: boolean
  pid?: number
}

interface Socks5Event {
  seq: number
  at: string
  type: string
  port?: number
  sessionId?: string
  command?: string
  target?: string
  peer?: string
  status?: number
  error?: string
}

interface Socks5Session {
  id: string
  serverId: string
  command: string
  target: string
  peer: string
  state: 'established' | 'failed' | 'closed' | 'error'
  status: number
  error?: string
  at: string
  endedAt?: string
  durationMs?: number
  bytesClientToTarget?: number
  bytesTargetToClient?: number
  udpDatagrams?: number
  udpBytes?: number
  endStatus?: number
  bytesAvailable?: boolean
}

const i18n = useI18nStore()
const t = i18n.t
const ui = useUiStore()

const name = ref('')
const host = ref('127.0.0.1')
const port = ref(1080)
const username = ref('')
const password = ref('')
const autoStart = ref(false)
const startNow = ref(true)
const isolated = ref(false)
const creating = ref(false)

const servers = ref<Socks5ServerInfo[]>([])
const selectedId = ref('')
const events = ref<Socks5Event[]>([])
const eventSeq = ref(0)
const sessions = ref<Socks5Session[]>([])
const eventLog = ref<HTMLElement | null>(null)

let timer: number | undefined
let inFlight = false
let again = false

const selected = computed(
  () => servers.value.find((item) => item.id === selectedId.value) || null,
)

function stateLabel(state: string) {
  return t(`socks5.state.${state}`)
}

function eventLabel(type: string) {
  return t(`socks5.event.${type}`)
}

function sessionStateLabel(state: string) {
  return t(`socks5.session.${state}`)
}

function formatBytes(bytes = 0) {
  if (!bytes) return '0 B'
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`
}

function sessionDetail(session: Socks5Session) {
  const parts = [session.peer, sessionStateLabel(session.state)]
  if (session.state === 'closed' || session.state === 'error') {
    if (session.udpDatagrams) {
      parts.push(`${session.udpDatagrams} datagrams · ${formatBytes(session.udpBytes)}`)
    } else {
      parts.push(
        `↑ ${formatBytes(session.bytesClientToTarget)} / ↓ ${formatBytes(session.bytesTargetToClient)}`,
      )
    }
    if (session.durationMs != null) parts.push(`${session.durationMs} ms`)
  }
  if (session.error) parts.push(session.error)
  parts.push(formatTime(session.at))
  return parts.join(' · ')
}

function formatTime(value?: string) {
  if (!value) return '—'
  return new Date(value).toLocaleTimeString([], {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  })
}

function eventText(event: Socks5Event) {
  if (event.type === 'started') return event.port ? `:${event.port}` : ''
  if (event.type === 'error') return event.error || ''
  if (event.type === 'session' || event.type === 'session-failed') {
    return [event.command, event.target, event.peer].filter(Boolean).join(' · ')
  }
  return ''
}

function scrollEvents() {
  void nextTick(() => {
    if (eventLog.value) eventLog.value.scrollTop = eventLog.value.scrollHeight
  })
}

async function loadServers() {
  try {
    const response = await get<{ ok: boolean; servers: Socks5ServerInfo[] }>('/api/socks5/servers')
    if (!response.ok) return
    servers.value = response.servers
    if (selectedId.value && !servers.value.some((item) => item.id === selectedId.value)) {
      selectedId.value = ''
      events.value = []
      sessions.value = []
    }
  } catch {
    // Polling is best-effort.
  }
}

async function createServer() {
  if (!host.value.trim() || !port.value) {
    ui.notify('warning', t('socks5.notify.hostRequired'))
    return
  }
  if ((username.value.trim() && !password.value) || (!username.value.trim() && password.value)) {
    ui.notify('warning', t('socks5.notify.credentialsRequired'))
    return
  }
  creating.value = true
  try {
    const response = await post<{ ok: boolean; server?: string; error?: string }>(
      '/api/socks5/servers',
      {
        name: name.value.trim(),
        host: host.value.trim(),
        port: port.value,
        username: username.value,
        password: password.value,
        autoStart: autoStart.value,
        start: startNow.value,
        isolated: isolated.value,
      },
    )
    if (!response.ok || !response.server) {
      ui.notify('error', t('socks5.notify.createFailed'), response.error)
      return
    }
    password.value = ''
    selectServer(response.server)
    await refreshAll(true)
    ui.notify('success', t('socks5.notify.created'), `${host.value}:${port.value}`)
  } catch (err) {
    ui.toastError(err, t('socks5.notify.createFailed'))
  } finally {
    creating.value = false
  }
}

async function startServer(server: Socks5ServerInfo) {
  try {
    const response = await post<{ ok: boolean; error?: string }>(
      `/api/socks5/servers/${server.id}/start`,
    )
    if (!response.ok) ui.notify('error', t('socks5.notify.startFailed'), response.error)
    await refreshAll(true)
  } catch (err) {
    ui.toastError(err, t('socks5.notify.startFailed'))
  }
}

async function stopServer(server: Socks5ServerInfo) {
  try {
    const response = await post<{ ok: boolean; error?: string }>(
      `/api/socks5/servers/${server.id}/stop`,
    )
    if (!response.ok) ui.notify('error', t('socks5.notify.stopFailed'), response.error)
    await refreshAll(true)
  } catch (err) {
    ui.toastError(err, t('socks5.notify.stopFailed'))
  }
}

async function deleteServer(server: Socks5ServerInfo) {
  if (!window.confirm(t('socks5.confirm.delete'))) return
  try {
    const response = await del<{ ok: boolean; error?: string }>(
      `/api/socks5/servers/${server.id}`,
    )
    if (!response.ok) {
      ui.notify('error', t('socks5.notify.deleteFailed'), response.error)
      return
    }
    if (selectedId.value === server.id) selectedId.value = ''
    await refreshAll(true)
    ui.notify('success', t('socks5.notify.deleted'))
  } catch (err) {
    ui.toastError(err, t('socks5.notify.deleteFailed'))
  }
}

function selectServer(id: string) {
  selectedId.value = id
  events.value = []
  eventSeq.value = 0
  sessions.value = []
  void refreshAll(true)
}

async function pollEvents(server: Socks5ServerInfo) {
  try {
    const response = await get<{
      ok: boolean
      server: Socks5ServerInfo
      events: Socks5Event[]
    }>(`/api/socks5/servers/${server.id}/events?after=${eventSeq.value}&limit=200`)
    if (!response.ok) return
    if (response.events?.length) {
      events.value.push(...response.events)
      if (events.value.length > 1000) events.value = events.value.slice(-1000)
      eventSeq.value = response.events[response.events.length - 1].seq
      scrollEvents()
    }
    const index = servers.value.findIndex((item) => item.id === server.id)
    if (index >= 0) servers.value[index] = response.server
  } catch {
    // Polling is best-effort.
  }
}

async function loadSessions(server: Socks5ServerInfo) {
  try {
    const response = await get<{ ok: boolean; sessions: Socks5Session[] }>(
      `/api/socks5/servers/${server.id}/sessions`,
    )
    if (response.ok) sessions.value = response.sessions
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
    await loadServers()
    const server = selected.value
    if (!server) return
    await pollEvents(server)
    await loadSessions(server)
  } finally {
    inFlight = false
    if (again) {
      again = false
      void refreshAll(true)
    }
  }
}

function startPolling() {
  if (timer) return
  timer = window.setInterval(() => void refreshAll(), 1000)
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
        <span class="eyebrow">{{ t('socks5.eyebrow') }}</span>
        <h2>{{ t('socks5.title') }}</h2>
        <p>{{ t('socks5.lead') }}</p>
      </div>
      <span class="chip mono">SOCKS5 · RFC 1928</span>
    </header>

    <section class="card listener-create">
      <header class="card__head">
        <div><span class="eyebrow">{{ t('socks5.create.eyebrow') }}</span><h3>{{ t('socks5.create.title') }}</h3></div>
      </header>
      <div class="form-grid">
        <label class="field field--grow">
          <span>{{ t('socks5.create.name') }}</span>
          <input v-model="name" :placeholder="t('socks5.create.namePlaceholder')" spellcheck="false" />
        </label>
        <label class="field field--grow">
          <span>{{ t('socks5.create.host') }}</span>
          <input v-model="host" class="mono" spellcheck="false" />
        </label>
        <label class="field field--port">
          <span>{{ t('socks5.create.port') }}</span>
          <input v-model.number="port" type="number" min="1" max="65535" class="mono" />
        </label>
        <label class="field">
          <span>{{ t('socks5.create.username') }}</span>
          <input
            v-model="username"
            class="mono"
            :placeholder="t('socks5.create.usernamePlaceholder')"
            autocomplete="off"
            spellcheck="false"
          />
        </label>
        <label class="field">
          <span>{{ t('socks5.create.password') }}</span>
          <input
            v-model="password"
            type="password"
            class="mono"
            :placeholder="t('socks5.create.passwordPlaceholder')"
            autocomplete="new-password"
          />
        </label>
      </div>
      <div class="form-actions listener-create__actions">
        <label class="listener-check"><input v-model="autoStart" type="checkbox" />{{ t('socks5.create.autoStart') }}</label>
        <label class="listener-check"><input v-model="startNow" type="checkbox" />{{ t('socks5.create.startNow') }}</label>
        <label class="listener-check"><input v-model="isolated" type="checkbox" />{{ t('socks5.create.isolated') }}</label>
        <span class="muted">{{ t('socks5.create.authHint') }}</span>
        <button class="button button--primary" :disabled="creating" @click="createServer">
          <span v-if="creating" class="spinner" />
          <Icon v-else name="plus" :size="15" />
          {{ creating ? t('socks5.create.creating') : t('socks5.create.action') }}
        </button>
      </div>
    </section>

    <div class="listener-grid">
      <section class="card listener-list">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('socks5.list.eyebrow') }}</span><h3>{{ t('socks5.list.title') }}</h3></div>
          <button class="icon-button icon-button--sm" :title="t('socks5.list.refresh')" @click="refreshAll(true)">
            <Icon name="refresh" :size="15" />
          </button>
        </header>
        <div v-if="servers.length" class="listener-cards">
          <button
            v-for="server in servers"
            :key="server.id"
            class="listener-card"
            :class="{ 'listener-card--active': server.id === selectedId }"
            @click="selectServer(server.id)"
          >
            <span class="session-card__dot" :class="`session-card__dot--${server.state === 'listening' ? 'connected' : server.state}`" />
            <span class="listener-card__body">
              <strong class="mono">{{ server.host }}:{{ server.port }}</strong>
              <small>
                {{ stateLabel(server.state) }}
                <template v-if="server.authRequired"> · {{ t('socks5.facts.authEnabled') }}</template>
                <template v-if="server.name"> · {{ server.name }}</template>
              </small>
            </span>
            <span v-if="server.isolated" class="chip mono">child</span>
            <span class="chip mono">{{ server.authRequired ? 'auth' : 'open' }}</span>
          </button>
        </div>
        <div v-else class="session-empty">
          <Icon name="key" :size="20" />
          <span>{{ t('socks5.list.empty') }}</span>
        </div>
      </section>

      <section class="card listener-detail">
        <header class="card__head">
          <div>
            <span class="eyebrow">{{ t('socks5.events.eyebrow') }}</span>
            <h3>{{ selected ? `${selected.host}:${selected.port}` : t('socks5.list.select') }}</h3>
          </div>
          <div v-if="selected" class="listener-actions">
            <button
              v-if="selected.state !== 'listening'"
              class="button button--sm"
              @click="startServer(selected)"
            >{{ t('socks5.action.start') }}</button>
            <button v-else class="button button--sm" @click="stopServer(selected)">
              {{ t('socks5.action.stop') }}
            </button>
            <button class="icon-button icon-button--sm icon-button--danger" :title="t('socks5.action.delete')" @click="deleteServer(selected)">
              <Icon name="trash" :size="14" />
            </button>
          </div>
        </header>

        <div v-if="!selected" class="session-empty session-empty--large">
          <Icon name="key" :size="24" />
          <span>{{ t('socks5.list.select') }}</span>
        </div>

        <template v-else>
          <div class="listener-facts">
            <div><span>{{ t('socks5.facts.active') }}</span><strong>{{ selected.activeSessions ?? 0 }}</strong></div>
            <div><span>{{ t('socks5.facts.total') }}</span><strong>{{ selected.totalSessions ?? 0 }}</strong></div>
            <div><span>{{ t('socks5.facts.rejected') }}</span><strong>{{ selected.rejectedSessions ?? 0 }}</strong></div>
            <div><span>{{ t('socks5.facts.auth') }}</span><strong>{{ selected.authRequired ? `${t('socks5.facts.authEnabled')} · ${selected.username}` : t('socks5.facts.authNone') }}</strong></div>
            <div><span>{{ t('socks5.facts.autoStart') }}</span><strong>{{ selected.autoStart ? t('socks5.facts.yes') : t('socks5.facts.no') }}</strong></div>
            <div><span>{{ t('socks5.facts.startedAt') }}</span><strong class="mono">{{ formatTime(selected.startedAt) }}</strong></div>
          </div>
          <div v-if="selected.error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ selected.error }}</div>

          <div ref="eventLog" class="session-events listener-events">
            <div v-for="event in events" :key="event.seq" class="session-event" :class="`session-event--${event.type}`">
              <span class="session-event__seq mono">{{ event.seq }}</span>
              <span class="session-event__type">{{ eventLabel(event.type) }}</span>
              <span class="session-event__body mono">{{ eventText(event) }}</span>
              <span class="session-event__time mono">{{ formatTime(event.at) }}</span>
            </div>
            <div v-if="!events.length" class="session-empty"><span>{{ t('socks5.events.empty') }}</span></div>
          </div>

          <div class="listener-sessions">
            <div class="listener-sessions__head">
              <span class="eyebrow">{{ t('socks5.sessions.eyebrow') }}</span>
              <strong>{{ t('socks5.sessions.title') }}</strong>
            </div>
            <div v-if="sessions.length" class="listener-session-list">
              <div v-for="session in sessions" :key="session.id" class="listener-session">
                <span class="session-card__dot" :class="`session-card__dot--${session.state === 'established' ? 'connected' : session.state === 'closed' ? 'closed' : 'error'}`" />
                <span class="chip mono">{{ session.command }}</span>
                <span class="mono">{{ session.target || '—' }}</span>
                <span class="muted">{{ sessionDetail(session) }}</span>
              </div>
            </div>
            <div v-else class="session-empty"><span>{{ t('socks5.sessions.empty') }}</span></div>
            <span class="muted socks5-session-hint">{{ t('socks5.sessions.activeHint') }}</span>
          </div>
        </template>
      </section>
    </div>
  </div>
</template>
