<script setup lang="ts">
import { computed, ref } from 'vue'
import Icon from '../components/Icon.vue'
import TcpSessionPanel from '../components/TcpSessionPanel.vue'
import Socks5ProxyFields from '../components/Socks5ProxyFields.vue'
import { errorMessage, post } from '../api'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'

defineOptions({ name: 'TransportView' })

const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
const mode = ref<'tcp' | 'session' | 'udp'>('tcp')
const host = ref('example.com')
const port = ref(80)
const payload = ref('HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n')
const encoding = ref<'text' | 'hex'>('text')
const connectTimeoutMs = ref(8000)
const readTimeoutMs = ref(3000)
const udpTimeoutMs = ref(3000)
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
const sent = computed(() => (result.value?.sent as Record<string, unknown>) || null)
const received = computed(() => (result.value?.received as Record<string, unknown>) || null)
const timing = computed(() => (result.value?.timing as Record<string, unknown>) || {})

function switchMode(next: 'tcp' | 'session' | 'udp') {
  mode.value = next
  result.value = null
  error.value = ''
  if (next === 'session') return
  if (next === 'tcp') {
    host.value = 'example.com'
    port.value = 80
    payload.value = 'HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n'
  } else {
    host.value = '1.1.1.1'
    port.value = 53
    payload.value = ''
  }
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
    if (!result.value.ok) ui.notify('error', t('transport.notify.probeFailed'), String(result.value.error || ''))
  } catch (err) {
    error.value = errorMessage(err)
    ui.toastError(err, t('transport.notify.probeFailed'))
  } finally {
    running.value = false
  }
}

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
        </div>
      </div>
    </header>

    <TcpSessionPanel v-if="isSessionMode" />

    <div v-else class="transport-grid">
      <section class="card transport-form">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('transport.form.client', { mode: mode.toUpperCase() }) }}</span><h3>{{ t('transport.form.title') }}</h3></div>
          <span class="chip mono">{{ isTcp ? 'stream' : 'datagram' }}</span>
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

        <label class="field">
          <span>{{ t('transport.form.payload') }}</span>
          <textarea v-model="payload" class="code-input code-input--short" spellcheck="false" :placeholder="encoding === 'hex' ? '48 65 6c 6c 6f' : t('transport.form.payloadPlaceholder')" />
        </label>

        <Socks5ProxyFields v-model="proxy" />

        <div class="form-actions">
          <span class="muted">{{ t('transport.form.meta', { count: payload.length }) }}</span>
          <button class="button button--primary" :disabled="running" @click="probe">
            <span v-if="running" class="spinner" />
            <Icon v-else :name="isTcp ? 'bolt' : 'send'" :size="16" />
            {{ running ? t('transport.form.probing') : isTcp ? t('transport.form.openTcp') : t('transport.form.sendDatagram') }}
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
  </div>
</template>
