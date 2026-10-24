<script setup lang="ts">
import { computed, nextTick, onActivated, onBeforeUnmount, onDeactivated, onMounted, ref, watch } from 'vue'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import '@xterm/xterm/css/xterm.css'
import Icon from '../components/Icon.vue'
import { useRouter } from 'vue-router'
import { del, errorMessage, get, post } from '../api'
import { useAuthStore } from '../stores/auth'
import { useI18nStore } from '../stores/i18n'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'TerminalView' })

interface TerminalSession {
  id: string
  ownerId: number
  owner: string
  name: string
  shell: string
  cwd: string
  state: 'starting' | 'running' | 'closing' | 'exited' | 'error'
  error?: string
  exitCode?: number
  cols: number
  rows: number
  createdAt: string
  exitedAt?: string
  lastActivity?: string
  nextSeq?: number
  bufferedBytes?: number
}

interface Live {
  ws: WebSocket | null
  retry: number
  reconnectTimer?: number
  denied?: boolean
}

const auth = useAuthStore()
const ui = useUiStore()
const i18n = useI18nStore()
const router = useRouter()
const t = i18n.t

const sessions = ref<TerminalSession[]>([])
const openIds = ref<string[]>([])
const activeId = ref('')
const showAll = ref(false)
const loading = ref(true)
const createOpen = ref(false)
const creating = ref(false)
const form = ref({ name: '', shell: '', cwd: '', isolated: false })
const wsStates = ref<Record<string, string>>({})
const quickCommands = ref<Array<{
  id: number
  command: string
  nameEn: string
  nameZh: string
  descriptionEn: string
  descriptionZh: string
  category: string
}>>([])
const hostPlatform = /Windows/i.test(navigator.userAgent) ? 'windows' : 'linux'
const pendingInput = new Map<string, string>()

const terminals = new Map<string, Terminal>()
const fits = new Map<string, FitAddon>()
const live = new Map<string, Live>()
const hosts = new Map<string, HTMLElement>()
const observers = new Map<string, ResizeObserver>()
const resizeTimers = new Map<string, number>()

const isAdmin = computed(() => auth.user?.role === 'admin')
const canOperate = computed(() => auth.user?.role === 'admin' || auth.user?.role === 'operator')
const activeSession = computed(() => sessions.value.find((item) => item.id === activeId.value) || null)
const visibleSessions = computed(() =>
  sessions.value.filter((item) => showAll.value || item.ownerId === auth.user?.id),
)
const openSessions = computed(() =>
  openIds.value
    .map((id) => sessions.value.find((item) => item.id === id))
    .filter((item): item is TerminalSession => Boolean(item)),
)
const storageKey = computed(() => `uvscope-terminal-open:${auth.user?.id ?? 'anon'}`)

const darkTheme = {
  background: '#0b1018',
  foreground: '#d7e0ea',
  cursor: '#4be3c2',
  cursorAccent: '#0b1018',
  selectionBackground: '#294357',
  black: '#141b25',
  red: '#ff6b81',
  green: '#5ee2a0',
  yellow: '#f0c674',
  blue: '#7c9cff',
  magenta: '#d18aff',
  cyan: '#4be3c2',
  white: '#d7e0ea',
  brightBlack: '#64748b',
  brightRed: '#ff8a9b',
  brightGreen: '#82f0bb',
  brightYellow: '#ffd98e',
  brightBlue: '#a3b8ff',
  brightMagenta: '#e3b3ff',
  brightCyan: '#8af5df',
  brightWhite: '#ffffff',
}

const lightTheme = {
  background: '#fbfcfe',
  foreground: '#1f2937',
  cursor: '#0f766e',
  cursorAccent: '#fbfcfe',
  selectionBackground: '#c7d7ea',
  black: '#334155',
  red: '#b42318',
  green: '#047857',
  yellow: '#a16207',
  blue: '#1d4ed8',
  magenta: '#7e22ce',
  cyan: '#0f766e',
  white: '#475569',
  brightBlack: '#64748b',
  brightRed: '#dc2626',
  brightGreen: '#059669',
  brightYellow: '#ca8a04',
  brightBlue: '#2563eb',
  brightMagenta: '#9333ea',
  brightCyan: '#0d9488',
  brightWhite: '#0f172a',
}

const terminalTheme = computed(() => (ui.resolvedTheme === 'light' ? lightTheme : darkTheme))

function setHost(id: string, element: unknown) {
  if (element instanceof HTMLElement) hosts.set(id, element)
  else hosts.delete(id)
}

function setWsState(id: string, state: string) {
  wsStates.value = { ...wsStates.value, [id]: state }
}

function stateLabel(state: string) {
  return t(`terminal.state.${state}`)
}

function wsLabel(id: string) {
  const state = wsStates.value[id]
  return state ? t(`terminal.ws.${state}`) : t('terminal.ws.connecting')
}

function formatTime(value?: string) {
  if (!value) return ''
  return new Date(value).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
}

function wsUrl(id: string) {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${protocol}//${window.location.host}/api/terminal/ws/${id}`
}

function saveOpen() {
  try {
    localStorage.setItem(storageKey.value, JSON.stringify(openIds.value))
  } catch {
    // Session restoration is a convenience.
  }
}

function restoreOpen() {
  try {
    const parsed = JSON.parse(localStorage.getItem(storageKey.value) || '[]')
    if (!Array.isArray(parsed)) return []
    return parsed.filter((id) => typeof id === 'string')
  } catch {
    return []
  }
}

async function loadSessions() {
  try {
    const query = showAll.value && isAdmin.value ? '?all=1' : ''
    const response = await get<{ ok: boolean; sessions: TerminalSession[] }>(
      `/api/terminal/sessions${query}`,
    )
    if (!response.ok) return
    sessions.value = response.sessions
    const known = new Set(sessions.value.map((item) => item.id))
    const previous = [...openIds.value]
    openIds.value = openIds.value.filter((id) => known.has(id))
    for (const id of previous) {
      if (!openIds.value.includes(id)) disposeTerminal(id)
    }
    if (activeId.value && !known.has(activeId.value)) activeId.value = openIds.value[0] || ''
    if (!activeId.value && openIds.value.length) activeId.value = openIds.value[0]
  } catch {
    // Polling is best-effort.
  } finally {
    loading.value = false
  }
}

async function createTerminal() {
  if (!form.value.name.trim()) {
    ui.notify('warning', t('terminal.create.nameRequired'))
    return
  }
  creating.value = true
  try {
    const response = await post<{ ok: boolean; session?: TerminalSession; error?: string }>(
      '/api/terminal/sessions',
      {
        name: form.value.name.trim(),
        shell: form.value.shell.trim(),
        cwd: form.value.cwd.trim(),
        cols: 100,
        rows: 30,
        isolated: form.value.isolated,
      },
    )
    if (!response.ok || !response.session) {
      ui.notify('error', t('terminal.create.failed'), response.error)
      return
    }
    createOpen.value = false
    form.value = { name: '', shell: '', cwd: '', isolated: false }
    sessions.value = [response.session, ...sessions.value]
    await openTerminal(response.session.id)
    saveOpen()
  } catch (err) {
    ui.toastError(err, t('terminal.create.failed'))
  } finally {
    creating.value = false
  }
}

async function closeSession(session: TerminalSession) {
  if (!window.confirm(t('terminal.closeConfirm', { name: session.name }))) return
  try {
    const response = await del<{ ok: boolean; error?: string }>(
      `/api/terminal/sessions/${session.id}`,
    )
    if (!response.ok) {
      ui.notify('error', t('terminal.closeFailed'), response.error)
      return
    }
    disposeTerminal(session.id)
    openIds.value = openIds.value.filter((id) => id !== session.id)
    if (activeId.value === session.id) activeId.value = openIds.value[openIds.value.length - 1] || ''
    saveOpen()
    await loadSessions()
  } catch (err) {
    ui.toastError(err, t('terminal.closeFailed'))
  }
}

function connect(id: string) {
  const entry = live.get(id) ?? { ws: null, retry: 0 }
  if (entry.denied) return
  if (entry.ws && entry.ws.readyState <= WebSocket.OPEN) return
  if (!openIds.value.includes(id)) return
  const socket = new WebSocket(wsUrl(id))
  socket.binaryType = 'arraybuffer'
  entry.ws = socket
  live.set(id, entry)
  setWsState(id, 'connecting')

  socket.onopen = () => {
    entry.retry = 0
    setWsState(id, 'connected')
    fitTerminal(id)
    sendResize(id)
    const pending = pendingInput.get(id)
    if (pending) {
      pendingInput.delete(id)
      socket.send(JSON.stringify({ type: 'input', data: pending }))
    }
    terminals.get(id)?.focus()
  }
  socket.onmessage = (event) => {
    const term = terminals.get(id)
    if (!term) return
    if (typeof event.data === 'string') {
      let message: Record<string, unknown>
      try {
        message = JSON.parse(event.data)
      } catch {
        term.write(event.data)
        return
      }
      if (message.type === 'attached') {
        term.reset()
        setWsState(id, 'connected')
      } else if (message.type === 'reset') {
        term.reset()
      } else if (message.type === 'exit') {
        const code = Number(message.code ?? -1)
        setWsState(id, 'disconnected')
        term.write(`\r\n\x1b[2m[${t('terminal.exitedWith', { code })}]\x1b[0m\r\n`)
        void loadSessions()
      } else if (message.type === 'error') {
        term.write(`\r\n\x1b[31m${String(message.error || '')}\x1b[0m\r\n`)
      }
    } else if (event.data instanceof ArrayBuffer) {
      term.write(new Uint8Array(event.data))
    }
  }
  socket.onclose = (event) => {
    const current = live.get(id)
    if (current) current.ws = null
    if (event.code === 1008 || event.code === 1002) {
      entry.denied = true
      setWsState(id, 'denied')
      return
    }
    const session = sessions.value.find((item) => item.id === id)
    if (session && session.state === 'running' && openIds.value.includes(id)) {
      setWsState(id, 'reconnecting')
      entry.retry = Math.min(6, entry.retry + 1)
      window.clearTimeout(entry.reconnectTimer)
      entry.reconnectTimer = window.setTimeout(
        () => connect(id),
        Math.min(5000, 400 * entry.retry),
      )
    } else {
      setWsState(id, 'disconnected')
    }
  }
  socket.onerror = () => {
    setWsState(id, 'disconnected')
  }
}

function disconnect(id: string, permanent = false) {
  const entry = live.get(id)
  if (!entry) return
  if (permanent) entry.denied = true
  window.clearTimeout(entry.reconnectTimer)
  if (entry.ws) {
    entry.ws.onclose = null
    entry.ws.close()
    entry.ws = null
  }
  live.delete(id)
}

function sendInput(id: string, data: string) {
  const socket = live.get(id)?.ws
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    pendingInput.set(id, (pendingInput.get(id) || '') + data)
    return
  }
  socket.send(JSON.stringify({ type: 'input', data }))
}

function fitTerminal(id: string) {
  const host = hosts.get(id)
  const fit = fits.get(id)
  const term = terminals.get(id)
  if (!host || !fit || !term) return
  if (host.clientWidth < 20 || host.clientHeight < 20) return
  try {
    fit.fit()
    term.focus()
  } catch {
    // A hidden pane cannot be measured; the next activation retries.
  }
}

function sendResize(id: string) {
  const term = terminals.get(id)
  const socket = live.get(id)?.ws
  if (!term || !socket || socket.readyState !== WebSocket.OPEN) return
  socket.send(JSON.stringify({ type: 'resize', cols: term.cols, rows: term.rows }))
}

function attachClipboard(term: Terminal, id: string) {
  term.attachCustomKeyEventHandler((event) => {
    if (event.type !== 'keydown') return true
    if (!(event.ctrlKey || event.metaKey) || !event.shiftKey) return true
    if (event.code === 'KeyC') {
      const selection = term.getSelection()
      if (selection) void navigator.clipboard.writeText(selection).catch(() => {})
      return false
    }
    if (event.code === 'KeyV') {
      void navigator.clipboard
        .readText()
        .then((text) => sendInput(id, text))
        .catch(() => {})
      return false
    }
    return true
  })
}

async function ensureTerminal(session: TerminalSession) {
  if (!openIds.value.includes(session.id)) openIds.value.push(session.id)
  await nextTick()
  if (terminals.has(session.id)) return
  const host = hosts.get(session.id)
  if (!host) return
  const term = new Terminal({
    cursorBlink: true,
    fontFamily: "'JetBrains Mono', 'Cascadia Code', 'SFMono-Regular', Menlo, Consolas, monospace",
    fontSize: 13,
    lineHeight: 1.0,
    letterSpacing: 0,
    scrollback: 10000,
    allowProposedApi: true,
    convertEol: false,
    macOptionIsMeta: true,
    rightClickSelectsWord: true,
    theme: terminalTheme.value,
  })
  const fit = new FitAddon()
  term.loadAddon(fit)
  term.open(host)
  terminals.set(session.id, term)
  fits.set(session.id, fit)
  term.onData((data) => sendInput(session.id, data))
  term.onResize(() => sendResize(session.id))
  attachClipboard(term, session.id)
  term.resize(Math.max(20, session.cols), Math.max(5, session.rows))
  const observer = new ResizeObserver(() => {
    if (session.id !== activeId.value) return
    window.clearTimeout(resizeTimers.get(session.id))
    resizeTimers.set(
      session.id,
      window.setTimeout(() => {
        fitTerminal(session.id)
        sendResize(session.id)
      }, 120),
    )
  })
  observer.observe(host)
  observers.set(session.id, observer)
  connect(session.id)
  if (session.id === activeId.value) {
    await nextTick()
    fitTerminal(session.id)
  }
}

function disposeTerminal(id: string) {
  disconnect(id, true)
  observers.get(id)?.disconnect()
  observers.delete(id)
  const timer = resizeTimers.get(id)
  if (timer) window.clearTimeout(timer)
  resizeTimers.delete(id)
  terminals.get(id)?.dispose()
  terminals.delete(id)
  fits.delete(id)
  hosts.delete(id)
}

async function openTerminal(id: string) {
  const session = sessions.value.find((item) => item.id === id)
  if (!session) return
  activeId.value = id
  await ensureTerminal(session)
  await nextTick()
  fitTerminal(id)
  saveOpen()
}

function activate(id: string) {
  activeId.value = id
  saveOpen()
  void nextTick(() => {
    fitTerminal(id)
    sendResize(id)
  })
}

function reconnect(id: string) {
  const entry = live.get(id)
  if (entry) entry.denied = false
  connect(id)
}

async function loadQuickCommands() {
  try {
    const response = await get<{ ok: boolean; commands: Array<Record<string, unknown>> }>(
      `/api/terminal/commands?platform=${hostPlatform}`,
    )
    if (!response.ok) return
    quickCommands.value = response.commands
      .filter((item) => item.quick === true && typeof item.id === 'number')
      .map((item) => ({
        id: item.id as number,
        command: String(item.command || ''),
        nameEn: String(item.nameEn || item.command || ''),
        nameZh: String(item.nameZh || item.nameEn || item.command || ''),
        descriptionEn: String(item.descriptionEn || ''),
        descriptionZh: String(item.descriptionZh || item.descriptionEn || ''),
        category: String(item.category || ''),
      }))
      .slice(0, 18)
  } catch {
    quickCommands.value = []
  }
}

function commandName(command: { nameEn: string; nameZh: string }) {
  return i18n.locale === 'zh-CN' ? command.nameZh || command.nameEn : command.nameEn || command.nameZh
}

function runQuick(command: { command: string }) {
  const id = activeId.value
  const text = command.command.endsWith('\n') ? command.command : `${command.command}\n`
  if (id) {
    sendInput(id, text)
    terminals.get(id)?.focus()
    return
  }
  void navigator.clipboard.writeText(command.command).then(
    () => ui.notify('success', t('terminal.commands.copied'), command.command),
    () => ui.notify('warning', t('terminal.commands.copyFailed')),
  )
}

function openCommandLibrary() {
  void router.push('/commands')
}

function consumePendingCommand() {
  let pending = ''
  try {
    pending = localStorage.getItem('uvscope-terminal-pending-command') || ''
    if (pending) localStorage.removeItem('uvscope-terminal-pending-command')
  } catch {
    return
  }
  if (!pending) return
  const id = activeId.value
  if (!id) {
    void navigator.clipboard.writeText(pending).then(
      () => ui.notify('success', t('terminal.commands.copied'), pending),
      () => ui.notify('warning', t('terminal.commands.copyFailed')),
    )
    return
  }
  sendInput(id, pending.endsWith('\n') ? pending : `${pending}\n`)
  void nextTick(() => terminals.get(id)?.focus())
}

function openCreate() {
  form.value = { name: '', shell: '', cwd: '', isolated: false }
  createOpen.value = true
}

async function toggleAll() {
  showAll.value = !showAll.value
  await loadSessions()
}

let pollTimer: number | undefined

async function restoreSessions() {
  await loadSessions()
  const stored = restoreOpen().filter((id) => sessions.value.some((item) => item.id === id))
  openIds.value = stored
  activeId.value = stored[0] || ''
  for (const id of stored) {
    const session = sessions.value.find((item) => item.id === id)
    if (session) await ensureTerminal(session)
  }
  if (activeId.value) await nextTick(() => fitTerminal(activeId.value))
}

onMounted(() => {
  void restoreSessions().then(consumePendingCommand)
  void loadQuickCommands()
  pollTimer = window.setInterval(() => void loadSessions(), 1800)
})
onActivated(() => {
  void loadQuickCommands()
  void loadSessions().then(() => {
    for (const id of openIds.value) {
      if (!live.get(id)?.ws) connect(id)
    }
    if (activeId.value) void nextTick(() => fitTerminal(activeId.value))
    consumePendingCommand()
  })
  if (!pollTimer) pollTimer = window.setInterval(() => void loadSessions(), 1800)
})
onDeactivated(() => {
  if (pollTimer) window.clearInterval(pollTimer)
  pollTimer = undefined
})
onBeforeUnmount(() => {
  if (pollTimer) window.clearInterval(pollTimer)
  for (const id of [...terminals.keys()]) disposeTerminal(id)
})

watch(terminalTheme, (theme) => {
  for (const term of terminals.values()) term.options.theme = theme
})

watch(activeId, (id) => {
  if (!id) return
  void nextTick(() => {
    fitTerminal(id)
    sendResize(id)
  })
})
</script>

<template>
  <div class="page terminal-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('terminal.eyebrow') }}</span>
        <h2>{{ t('terminal.title') }}</h2>
        <p>{{ t('terminal.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <button v-if="isAdmin" class="button button--ghost" @click="toggleAll">
          <Icon name="users" :size="15" />{{ showAll ? t('terminal.onlyMine') : t('terminal.allUsers') }}
        </button>
        <button class="button button--primary" :disabled="!canOperate" @click="openCreate">
          <Icon name="plus" :size="16" />{{ t('terminal.new') }}
        </button>
      </div>
    </header>

    <div class="terminal-shell">
      <section class="card terminal-sidebar">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('terminal.sessions') }}</span><h3>{{ visibleSessions.length }}</h3></div>
        </header>
        <div v-if="loading" class="session-empty"><span class="spinner" /></div>
        <div v-else-if="visibleSessions.length" class="terminal-session-list">
          <button
            v-for="session in visibleSessions"
            :key="session.id"
            class="terminal-session"
            :class="{ 'terminal-session--active': session.id === activeId }"
            @click="openTerminal(session.id)"
          >
            <span class="session-card__dot" :class="`session-card__dot--${session.state === 'running' ? 'connected' : session.state === 'exited' ? 'closed' : 'error'}`" />
            <span class="terminal-session__body">
              <strong>{{ session.name }}</strong>
              <small>
                {{ stateLabel(session.state) }}
                <template v-if="showAll"> · {{ session.owner }}</template>
                <template v-if="session.exitedAt"> · {{ formatTime(session.exitedAt) }}</template>
              </small>
            </span>
            <span
              class="icon-button icon-button--sm"
              role="button"
              tabindex="0"
              :title="t('terminal.close')"
              @click.stop="closeSession(session)"
              @keydown.enter.stop="closeSession(session)"
            >
              <Icon name="trash" :size="13" />
            </span>
          </button>
        </div>
        <div v-else class="session-empty">
          <Icon name="terminal" :size="20" />
          <span>{{ t('terminal.empty') }}</span>
        </div>
      </section>

      <section class="card terminal-main">
        <div class="terminal-tabs">
          <button
            v-for="session in openSessions"
            :key="session.id"
            class="terminal-tab"
            :class="{ 'terminal-tab--active': session.id === activeId }"
            @click="activate(session.id)"
          >
            <span class="session-card__dot" :class="`session-card__dot--${session.state === 'running' ? 'connected' : session.state === 'exited' ? 'closed' : 'error'}`" />
            <span>{{ session.name }}</span>
            <small v-if="wsStates[session.id] && wsStates[session.id] !== 'connected'">{{ wsLabel(session.id) }}</small>
            <span class="terminal-tab__close" @click.stop="closeSession(session)"><Icon name="close" :size="12" /></span>
          </button>
          <span v-if="!openSessions.length" class="muted terminal-tabs__empty">{{ t('terminal.select') }}</span>
        </div>

        <div v-if="activeSession" class="terminal-statusbar">
          <span class="chip mono">{{ activeSession.shell }}</span>
          <span class="chip mono">{{ activeSession.cwd }}</span>
          <span class="chip" :class="activeSession.state === 'running' ? 'chip--success' : 'chip--warn'">{{ stateLabel(activeSession.state) }}</span>
          <span class="muted">{{ wsLabel(activeSession.id) }}</span>
          <span v-if="activeSession.exitCode != null && activeSession.exitCode >= 0" class="muted mono">{{ t('terminal.exitedWith', { code: activeSession.exitCode }) }}</span>
          <button v-if="wsStates[activeSession.id] === 'denied' || wsStates[activeSession.id] === 'disconnected'" class="link-button" @click="reconnect(activeSession.id)">
            {{ t('terminal.reconnect') }}
          </button>
          <span class="muted terminal-statusbar__hint">{{ t('terminal.openHint') }}</span>
        </div>

        <div class="terminal-quickbar">
          <span class="eyebrow">{{ t('terminal.quick.title') }}</span>
          <button
            v-for="command in quickCommands"
            :key="command.id"
            class="quick-chip"
            :title="i18n.locale === 'zh-CN' ? command.descriptionZh : command.descriptionEn"
            @click="runQuick(command)"
          >{{ commandName(command) }}</button>
          <button class="link-button terminal-quickbar__library" @click="openCommandLibrary">
            <Icon name="search" :size="13" />{{ t('terminal.commands.open') }}
          </button>
        </div>

        <div class="terminal-panes">
          <div
            v-for="session in openSessions"
            v-show="session.id === activeId"
            :key="session.id"
            class="terminal-pane"
            :style="{ background: terminalTheme.background }"
          >
            <div :ref="(el) => setHost(session.id, el)" class="terminal-host" />
          </div>
          <div v-if="!openSessions.length" class="terminal-empty">
            <Icon name="terminal" :size="28" />
            <p>{{ t('terminal.empty') }}</p>
          </div>
        </div>
      </section>
    </div>

    <Teleport to="body">
      <Transition name="modal">
        <div v-if="createOpen" class="modal-scrim" @click.self="createOpen = false">
          <form class="modal-card modal-card--sm" @submit.prevent="createTerminal">
            <header class="modal-head">
              <div><span class="eyebrow">{{ t('terminal.eyebrow') }}</span><h3>{{ t('terminal.create.title') }}</h3></div>
              <button type="button" class="icon-button" @click="createOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <label class="field"><span>{{ t('terminal.create.name') }}</span><input v-model="form.name" :placeholder="t('terminal.create.namePlaceholder')" autofocus /></label>
              <label class="field"><span>{{ t('terminal.create.shell') }}</span><input v-model="form.shell" class="mono" :placeholder="t('terminal.create.shellPlaceholder')" /></label>
              <label class="field"><span>{{ t('terminal.create.cwd') }}</span><input v-model="form.cwd" class="mono" :placeholder="t('terminal.create.cwdPlaceholder')" /></label>
              <label class="listener-check"><input v-model="form.isolated" type="checkbox" />{{ t('terminal.create.isolated') }}</label>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="createOpen = false">{{ t('terminal.create.cancel') }}</button>
              <button type="submit" class="button button--primary" :disabled="creating">
                <span v-if="creating" class="spinner" /><Icon v-else name="terminal" :size="15" />
                {{ creating ? t('terminal.create.creating') : t('terminal.create.action') }}
              </button>
            </footer>
          </form>
        </div>
      </Transition>
    </Teleport>
  </div>
</template>
