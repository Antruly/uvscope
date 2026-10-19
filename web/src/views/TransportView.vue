<script setup lang="ts">
import { computed, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { errorMessage, post } from '../api'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'TransportView' })

const ui = useUiStore()
const mode = ref<'tcp' | 'udp'>('tcp')
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

const isTcp = computed(() => mode.value === 'tcp')
const sent = computed(() => (result.value?.sent as Record<string, unknown>) || null)
const received = computed(() => (result.value?.received as Record<string, unknown>) || null)
const timing = computed(() => (result.value?.timing as Record<string, unknown>) || {})

function switchMode(next: 'tcp' | 'udp') {
  mode.value = next
  result.value = null
  error.value = ''
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
    ui.notify('warning', 'Host and port are required')
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
        }
      : {
          host: host.value.trim(),
          port: port.value,
          payload: payload.value,
          encoding: encoding.value,
          timeoutMs: udpTimeoutMs.value,
        }
    result.value = await post<Record<string, unknown>>(path, body)
    if (!result.value.ok) ui.notify('error', 'Probe failed', String(result.value.error || ''))
  } catch (err) {
    error.value = errorMessage(err)
    ui.toastError(err, 'Probe failed')
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
        <span class="eyebrow">Transport bench</span>
        <h2>Bytes, without the ceremony.</h2>
        <p>Open a one-shot TCP connection or fire a UDP datagram through the libuvcpp net layer.</p>
      </div>
      <div class="page-head__actions">
        <div class="segmented">
          <button :class="{ active: mode === 'tcp' }" @click="switchMode('tcp')">TCP</button>
          <button :class="{ active: mode === 'udp' }" @click="switchMode('udp')">UDP</button>
        </div>
      </div>
    </header>

    <div class="transport-grid">
      <section class="card transport-form">
        <header class="card__head">
          <div><span class="eyebrow">{{ mode.toUpperCase() }} client</span><h3>One-shot probe</h3></div>
          <span class="chip mono">{{ isTcp ? 'stream' : 'datagram' }}</span>
        </header>

        <div class="form-grid">
          <label class="field field--grow"><span>Host</span><input v-model="host" class="mono" spellcheck="false" placeholder="127.0.0.1" /></label>
          <label class="field field--port"><span>Port</span><input v-model.number="port" type="number" min="1" max="65535" class="mono" /></label>
        </div>

        <div class="form-grid">
          <label class="field">
            <span>Payload encoding</span>
            <select v-model="encoding" class="select">
              <option value="text">UTF-8 text</option>
              <option value="hex">Hex bytes</option>
            </select>
          </label>
          <label v-if="isTcp" class="field"><span>Connect timeout</span><input v-model.number="connectTimeoutMs" type="number" min="200" class="mono" /></label>
          <label v-if="isTcp" class="field"><span>Read timeout</span><input v-model.number="readTimeoutMs" type="number" min="0" class="mono" /></label>
          <label v-else class="field"><span>Receive timeout</span><input v-model.number="udpTimeoutMs" type="number" min="100" class="mono" /></label>
        </div>

        <label class="field">
          <span>Payload</span>
          <textarea v-model="payload" class="code-input code-input--short" spellcheck="false" :placeholder="encoding === 'hex' ? '48 65 6c 6c 6f' : 'Bytes to send…'" />
        </label>

        <div class="form-actions">
          <span class="muted">{{ payload.length }} chars · encrypted transport is configured per endpoint</span>
          <button class="button button--primary" :disabled="running" @click="probe">
            <span v-if="running" class="spinner" />
            <Icon v-else :name="isTcp ? 'bolt' : 'send'" :size="16" />
            {{ running ? 'Probing…' : isTcp ? 'Open TCP connection' : 'Send datagram' }}
          </button>
        </div>
        <div class="inline-alert inline-alert--info">
          <Icon name="spark" :size="16" />
          <span>Persistent TCP/UDP listeners and raw QUIC streams are planned for 0.2. This bench is a one-shot probe.</span>
        </div>
      </section>

      <section class="card transport-result">
        <header class="card__head">
          <div><span class="eyebrow">Result</span><h3>Wire transcript</h3></div>
          <div v-if="result" class="status-pill" :class="result.ok ? 'status-pill--ok' : 'status-pill--bad'">
            {{ result.ok ? 'COMPLETE' : 'FAILED' }}
          </div>
        </header>

        <div v-if="error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ error }}</div>
        <div v-else-if="!result" class="transport-empty">
          <div class="response-empty__art"><Icon name="layers" :size="28" /></div>
          <h3>No wire data yet</h3>
          <p>Send a probe to inspect raw bytes, timing and the peer response.</p>
        </div>
        <template v-else>
          <div class="wire-meta">
            <div><span>Target</span><strong class="mono">{{ result.host }}:{{ result.port }}</strong></div>
            <div><span>Elapsed</span><strong>{{ timing.totalMs ?? '—' }} ms</strong></div>
            <div v-if="isTcp"><span>Connect</span><strong>{{ timing.connectMs ?? '—' }} ms</strong></div>
            <div><span>Read</span><strong>{{ result.readTimedOut ? 'timed out' : 'complete' }}</strong></div>
          </div>
          <div v-if="result.error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ result.error }}</div>
          <div class="wire-block">
            <div class="wire-block__head"><span><Icon name="upload" :size="14" />Sent</span><span class="mono">{{ sent?.length ?? 0 }} B</span></div>
            <pre>{{ displayPayload(sent) || '(empty payload)' }}</pre>
          </div>
          <div class="wire-block wire-block--receive">
            <div class="wire-block__head"><span><Icon name="download" :size="14" />Received</span><span class="mono">{{ received?.length ?? 0 }} B</span></div>
            <pre>{{ displayPayload(received) || (result.readTimedOut ? 'No datagram before the receive timeout.' : '(no bytes)') }}</pre>
          </div>
          <div class="hex-note">
            <span class="eyebrow">Hex</span>
            <pre>{{ received?.hex || sent?.hex || 'No bytes to display.' }}</pre>
          </div>
        </template>
      </section>
    </div>
  </div>
</template>
