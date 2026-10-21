<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import TcpSessionPanel from '../components/TcpSessionPanel.vue'
import Socks5ProxyFields from '../components/Socks5ProxyFields.vue'
import { del, errorMessage, get, post } from '../api'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'

defineOptions({ name: 'TransportView' })

const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
const mode = ref<'tcp' | 'session' | 'udp' | 'quic'>('tcp')
const host = ref('example.com')
const port = ref(80)
const payload = ref('HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n')
const encoding = ref<'text' | 'hex'>('text')
const connectTimeoutMs = ref(8000)
const readTimeoutMs = ref(3000)
const udpTimeoutMs = ref(3000)
const alpn = ref('h3')
const verifyQuic = ref(false)
const running = ref(false)
const result = ref<Record<string, unknown> | null>(null)
const error = ref('')
const proxy = ref({ enabled: false, host: '127.0.0.1', port: 1080, username: '', password: '' })

function proxyPayload() {
  if (!proxy.value.enabled) return undefined
  return {
    host: proxy.value.host.trim(),
    port: proxy.value.port,
    username: proxy.value.username,
    password: proxy.value.password,
  }
}

const isTcp = computed(() => mode.value === 'tcp')
const isSessionMode = computed(() => mode.value === 'session')
const isQuic = computed(() => mode.value === 'quic')
const sent = computed(() => (result.value?.sent as Record<string, unknown>) || null)
const received = computed(() => (result.value?.received as Record<string, unknown>) || null)
const timing = computed(() => (result.value?.timing as Record<string, unknown>) || {})

function switchMode(next: 'tcp' | 'session' | 'udp' | 'quic') {
  mode.value = next
  result.value = null
  error.value = ''
  if (next === 'session') return
  if (next === 'tcp') {
    host.value = 'example.com'
    port.value = 80
    payload.value = 'HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n'
  } else if (next === 'quic') {
    host.value = '127.0.0.1'
    port.value = 4433
    payload.value = 'hello'
  } else {
    host.value = '1.1.1.1'
    port.value = 53
    payload.value = ''
  }
}

function bytesHex(text: string): string {
  return [...new TextEncoder().encode(text)]
    .map((byte) => byte.toString(16).padStart(2, '0'))
    .join(' ')
}

async function probe() {
  if (!host.value.trim() || !port.value) {
    ui.notify('warning', t('transport.notify.hostPortRequired'))
    return
  }
  running.value = true
  error.value = ''
  result.value = null
  try {
    if (isQuic.value) {
      const response = await post<Record<string, unknown>>('/api/quic/probe', {
        host: host.value.trim(),
        port: port.value,
        alpn: alpn.value.trim() || 'h3',
        payload: payload.value,
        encoding: encoding.value,
        timeoutMs: udpTimeoutMs.value,
        verify: verifyQuic.value,
      })
      const reply = String(response.replyText || response.replyBase64 || '')
      result.value = {
        ...response,
        sent: {
          length: payload.value.length,
          text: payload.value,
          hex: bytesHex(payload.value),
        },
        received: {
          length: response.replySize ?? 0,
          text: response.replyText ?? '',
          base64: response.replyBase64 ?? '',
          hex: response.replyText ? bytesHex(String(response.replyText)) : '',
        },
        readTimedOut: false,
      }
    } else {
      const path = isTcp.value ? '/api/tcp/probe' : '/api/udp/probe'
      const body = isTcp.value
        ? {
            host: host.value.trim(),
            port: port.value,
            payload: payload.value,
            encoding: encoding.value,
            connectTimeoutMs: connectTimeoutMs.value,
            readTimeoutMs: readTimeoutMs.value,
            proxy: proxyPayload(),
          }
        : {
            host: host.value.trim(),
            port: port.value,
            payload: payload.value,
            encoding: encoding.value,
            timeoutMs: udpTimeoutMs.value,
            proxy: proxyPayload(),
          }
      result.value = await post<Record<string, unknown>>(path, body)
    }
    if (!result.value.ok) ui.notify('error', t('transport.notify.probeFailed'), String(result.value.error || ''))
  } catch (err) {
    error.value = errorMessage(err)
    ui.toastError(err, t('transport.notify.probeFailed'))
  } finally {
    running.value = false
  }
}

const quicServers = ref<Array<Record<string, unknown>>>([])
const quicAvailable = ref(true)
const quicReason = ref('')
const quicForm = ref({ name: 'quic-echo', host: '127.0.0.1', port: 4433, alpn: 'h3', certPath: '', keyPath: '', echo: true })
const quicBusy = ref(false)

async function loadQuicServers() {
  try {
    const response = await get<Record<string, unknown>>('/api/quic/servers')
    quicServers.value = (response.servers as Array<Record<string, unknown>>) || []
    quicAvailable.value = response.available !== false
    quicReason.value = String(response.reason || '')
  } catch {
    // Capability reporting is best-effort.
  }
}

async function createQuicServer(start: boolean) {
  if (quicBusy.value) return
  quicBusy.value = true
  try {
    const response = await post<Record<string, unknown>>('/api/quic/servers', {
      ...quicForm.value,
      start,
    })
    if (response.warning) ui.notify('error', t('transport.quic.startFailed'), String(response.warning))
    if (response.ok) ui.notify('success', t('transport.quic.created'))
    await loadQuicServers()
  } catch (err) {
    ui.toastError(err, t('transport.quic.createFailed'))
  } finally {
    quicBusy.value = false
  }
}

async function startQuicServer(id: string) {
  try {
    await post(`/api/quic/servers/${id}/start`)
    await loadQuicServers()
  } catch (err) {
    ui.toastError(err, t('transport.quic.startFailed'))
  }
}

async function stopQuicServer(id: string) {
  try {
    await post(`/api/quic/servers/${id}/stop`)
    await loadQuicServers()
  } catch (err) {
    ui.toastError(err, t('transport.quic.stopFailed'))
  }
}

async function deleteQuicServer(id: string) {
  try {
    await del(`/api/quic/servers/${id}`)
    await loadQuicServers()
  } catch (err) {
    ui.toastError(err, t('transport.quic.deleteFailed'))
  }
}

onMounted(() => void loadQuicServers())

function displayPayload(value: Record<string, unknown> | null) {
  if (!value) return ''
  return String(value.text || value.hex || value.base64 || '')
}
</script>

<template>
  <div class="page transport-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('transport.header.eyebrow') }}</span>
        <h2>{{ t('transport.header.title') }}</h2>
        <p>{{ isSessionMode ? t('transport.header.leadSession') : t('transport.header.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <div class="segmented">
          <button :class="{ active: mode === 'tcp' }" @click="switchMode('tcp')">TCP</button>
          <button :class="{ active: mode === 'session' }" @click="switchMode('session')">{{ t('transport.mode.session') }}</button>
          <button :class="{ active: mode === 'udp' }" @click="switchMode('udp')">UDP</button>
          <button :class="{ active: mode === 'quic' }" @click="switchMode('quic')">QUIC <em>{{ t('transport.quic.experimental') }}</em></button>
        </div>
      </div>
    </header>

    <TcpSessionPanel v-if="isSessionMode" />

    <div v-else class="transport-grid">
      <section class="card transport-form">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('transport.form.client', { mode: mode.toUpperCase() }) }}</span><h3>{{ t('transport.form.title') }}</h3></div>
          <span class="chip mono">{{ isTcp || isQuic ? 'stream' : 'datagram' }}</span>
        </header>

        <div class="form-grid">
          <label class="field field--grow"><span>{{ t('transport.form.host') }}</span><input v-model="host" class="mono" spellcheck="false" placeholder="127.0.0.1" /></label>
          <label class="field field--port"><span>{{ t('transport.form.port') }}</span><input v-model.number="port" type="number" min="1" max="65535" class="mono" /></label>
        </div>

        <div class="form-grid">
          <label class="field">
            <span>{{ t('transport.form.encoding') }}</span>
            <select v-model="encoding" class="select">
              <option value="text">{{ t('transport.form.encodingText') }}</option>
              <option value="hex">{{ t('transport.form.encodingHex') }}</option>
            </select>
          </label>
          <label v-if="isTcp" class="field"><span>{{ t('transport.form.connectTimeout') }}</span><input v-model.number="connectTimeoutMs" type="number" min="200" class="mono" /></label>
          <label v-if="isTcp" class="field"><span>{{ t('transport.form.readTimeout') }}</span><input v-model.number="readTimeoutMs" type="number" min="0" class="mono" /></label>
          <label v-else class="field"><span>{{ t('transport.form.receiveTimeout') }}</span><input v-model.number="udpTimeoutMs" type="number" min="100" class="mono" /></label>
        </div>

        <div v-if="isQuic" class="form-grid">
          <label class="field"><span>{{ t('transport.form.alpn') }}</span><input v-model="alpn" class="mono" placeholder="h3" spellcheck="false" /></label>
          <label class="switch-field">
            <span>{{ t('transport.form.verify') }}</span>
            <button class="switch" :class="{ on: verifyQuic }" @click="verifyQuic = !verifyQuic"><i /></button>
          </label>
        </div>

        <label class="field">
          <span>{{ t('transport.form.payload') }}</span>
          <textarea v-model="payload" class="code-input code-input--short" spellcheck="false" :placeholder="encoding === 'hex' ? '48 65 6c 6c 6f' : t('transport.form.payloadPlaceholder')" />
        </label>

        <Socks5ProxyFields v-if="!isQuic" v-model="proxy" />

        <div class="form-actions">
          <span class="muted">{{ t('transport.form.meta', { count: payload.length }) }}</span>
          <button class="button button--primary" :disabled="running" @click="probe">
            <span v-if="running" class="spinner" />
            <Icon v-else :name="isTcp || isQuic ? 'bolt' : 'send'" :size="16" />
            {{ running ? t('transport.form.probing') : isQuic ? t('transport.form.quicProbe') : isTcp ? t('transport.form.openTcp') : t('transport.form.sendDatagram') }}
          </button>
        </div>
        <div class="inline-alert inline-alert--info">
          <Icon name="spark" :size="16" />
          <span>{{ t('transport.form.info') }}</span>
        </div>
      </section>

      <section class="card transport-result">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('transport.result.eyebrow') }}</span><h3>{{ t('transport.result.title') }}</h3></div>
          <div v-if="result" class="status-pill" :class="result.ok ? 'status-pill--ok' : 'status-pill--bad'">
            {{ result.ok ? t('transport.result.complete') : t('transport.result.failed') }}
          </div>
        </header>

        <div v-if="error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ error }}</div>
        <div v-else-if="!result" class="transport-empty">
          <div class="response-empty__art"><Icon name="layers" :size="28" /></div>
          <h3>{{ t('transport.empty.title') }}</h3>
          <p>{{ t('transport.empty.body') }}</p>
        </div>
        <template v-else>
          <div class="wire-meta">
            <div><span>{{ t('transport.meta.target') }}</span><strong class="mono">{{ result.host }}:{{ result.port }}</strong></div>
            <div><span>{{ t('transport.meta.elapsed') }}</span><strong>{{ timing.totalMs ?? '—' }} ms</strong></div>
            <div v-if="isTcp"><span>{{ t('transport.meta.connect') }}</span><strong>{{ timing.connectMs ?? '—' }} ms</strong></div>
            <div v-if="isQuic"><span>{{ t('transport.form.alpn') }}</span><strong class="mono">{{ result.negotiatedAlpn || alpn }}</strong></div>
            <div v-if="isQuic"><span>{{ t('transport.quic.streamId') }}</span><strong class="mono">{{ result.streamId ?? '—' }}</strong></div>
            <div><span>{{ t('transport.meta.read') }}</span><strong>{{ result.readTimedOut ? t('transport.meta.timedOut') : t('transport.meta.complete') }}</strong></div>
          </div>
          <div v-if="result.error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ result.error }}</div>
          <div class="wire-block">
            <div class="wire-block__head"><span><Icon name="upload" :size="14" />{{ t('transport.wire.sent') }}</span><span class="mono">{{ sent?.length ?? 0 }} B</span></div>
            <pre>{{ displayPayload(sent) || t('transport.wire.emptyPayload') }}</pre>
          </div>
          <div class="wire-block wire-block--receive">
            <div class="wire-block__head"><span><Icon name="download" :size="14" />{{ t('transport.wire.received') }}</span><span class="mono">{{ received?.length ?? 0 }} B</span></div>
            <pre>{{ displayPayload(received) || (result.readTimedOut ? t('transport.wire.noDatagram') : t('transport.wire.noBytes')) }}</pre>
          </div>
          <div class="hex-note">
            <span class="eyebrow">Hex</span>
            <pre>{{ received?.hex || sent?.hex || t('transport.hex.noBytes') }}</pre>
          </div>
        </template>
      </section>
    </div>

    <section v-if="isQuic" class="card transport-quic-servers">
      <header class="card__head">
        <div><span class="eyebrow">{{ t('transport.quic.eyebrow') }}</span><h3>{{ t('transport.quic.title') }}</h3></div>
        <button class="ghost-toggle" @click="loadQuicServers"><Icon name="refresh" :size="14" />{{ t('transport.quic.refresh') }}</button>
      </header>
      <div v-if="!quicAvailable" class="inline-alert inline-alert--warning">
        <Icon name="alert" :size="16" />{{ quicReason || t('transport.quic.unavailable') }}
      </div>
      <template v-else>
        <div class="form-grid">
          <label class="field"><span>{{ t('transport.quic.name') }}</span><input v-model="quicForm.name" /></label>
          <label class="field"><span>{{ t('transport.form.host') }}</span><input v-model="quicForm.host" class="mono" /></label>
          <label class="field"><span>{{ t('transport.form.port') }}</span><input v-model.number="quicForm.port" type="number" min="1" max="65535" class="mono" /></label>
          <label class="field"><span>{{ t('transport.form.alpn') }}</span><input v-model="quicForm.alpn" class="mono" /></label>
        </div>
        <div class="form-grid">
          <label class="field"><span>{{ t('transport.quic.certPath') }}</span><input v-model="quicForm.certPath" class="mono" :placeholder="t('transport.quic.selfSigned')" /></label>
          <label class="field"><span>{{ t('transport.quic.keyPath') }}</span><input v-model="quicForm.keyPath" class="mono" :placeholder="t('transport.quic.selfSigned')" /></label>
          <label class="switch-field">
            <span>{{ t('transport.quic.echo') }}</span>
            <button class="switch" :class="{ on: quicForm.echo }" @click="quicForm.echo = !quicForm.echo"><i /></button>
          </label>
          <div class="form-actions">
            <button class="button button--primary" :disabled="quicBusy" @click="createQuicServer(true)">
              <Icon name="play" :size="15" />{{ t('transport.quic.startServer') }}
            </button>
          </div>
        </div>
        <div v-if="!quicServers.length" class="proxy-rules-empty">{{ t('transport.quic.empty') }}</div>
        <div v-for="server in quicServers" :key="String(server.id)" class="user-row">
          <div class="user-cell">
            <strong>{{ server.name }}</strong>
            <small class="mono">{{ server.host }}:{{ server.boundPort || server.port }} · {{ server.state }}</small>
          </div>
          <div class="mono">{{ server.alpn }}</div>
          <div class="mono">{{ server.connections }} conn · {{ server.streams }} streams · {{ server.datagrams }} dgrams</div>
          <div class="user-actions">
            <button class="button" @click="startQuicServer(String(server.id))">{{ t('transport.quic.start') }}</button>
            <button class="button" @click="stopQuicServer(String(server.id))">{{ t('transport.quic.stop') }}</button>
            <button class="icon-button icon-button--sm icon-button--danger" @click="deleteQuicServer(String(server.id))"><Icon name="trash" :size="14" /></button>
          </div>
        </div>
      </template>
    </section>
  </div>
</template>
