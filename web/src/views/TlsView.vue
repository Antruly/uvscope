<script setup lang="ts">
import { computed, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { errorMessage, post } from '../api'
import type { CertificateInfo } from '../types'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'

defineOptions({ name: 'TlsView' })

const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
const pem = ref('')
const info = ref<CertificateInfo | null>(null)
const error = ref('')
const parsing = ref(false)
const fileInput = ref<HTMLInputElement | null>(null)
const inputEncoding = ref<'text' | 'base64'>('text')
const liveUrl = ref('https://')
const liveLoading = ref(false)
const liveError = ref('')

const validityPercent = computed(() => {
  if (!info.value || !info.value.notBeforeEpoch || !info.value.notAfterEpoch) return 0
  const now = Date.now() / 1000
  const total = info.value.notAfterEpoch - info.value.notBeforeEpoch
  if (total <= 0) return 0
  return Math.max(0, Math.min(100, ((now - info.value.notBeforeEpoch) / total) * 100))
})

const validityTone = computed(() => {
  if (!info.value) return 'ok'
  if (info.value.expired || info.value.notYetValid) return 'bad'
  if (info.value.daysRemaining < 30) return 'warn'
  return 'ok'
})

async function parse() {
  if (!pem.value.trim()) {
    ui.notify('warning', t('tls.msg.needInput'))
    return
  }
  parsing.value = true
  error.value = ''
  liveError.value = ''
  info.value = null
  if (pem.value.includes('-----BEGIN CERTIFICATE-----')) inputEncoding.value = 'text'
  try {
    info.value = await post<CertificateInfo>('/api/tls/inspect', { pem: pem.value, encoding: inputEncoding.value })
    ui.notify('success', t('tls.msg.decoded'))
  } catch (err) {
    error.value = errorMessage(err)
    ui.toastError(err, t('tls.msg.parseFailed'))
  } finally {
    parsing.value = false
  }
}

async function fetchLive() {
  const url = liveUrl.value.trim()
  if (!url || url === 'https://') {
    ui.notify('warning', t('tls.live.urlRequired'))
    return
  }
  liveLoading.value = true
  liveError.value = ''
  try {
    const response = await post<CertificateInfo>('/api/tls/probe', { url, timeoutMs: 10000 })
    if (!response.ok) {
      liveError.value = response.error || t('tls.live.failed')
      if (!response.certificates?.length) {
        info.value = null
      }
      return
    }
    info.value = response
    ui.notify(
      'success',
      response.live?.verified ? t('tls.live.verified') : t('tls.live.unverified'),
      `${response.host}:${response.port}`,
    )
  } catch (err) {
    liveError.value = errorMessage(err)
    ui.toastError(err, t('tls.live.failed'))
  } finally {
    liveLoading.value = false
  }
}

async function loadFile(event: Event) {
  const target = event.target as HTMLInputElement
  const file = target.files?.[0]
  if (!file) return
  if (file.size > 1024 * 1024) {
    ui.notify('warning', t('tls.msg.fileTooLarge'))
    return
  }
  const bytes = new Uint8Array(await file.arrayBuffer())
  const text = new TextDecoder('utf-8', { fatal: false }).decode(bytes)
  if (text.includes('-----BEGIN CERTIFICATE-----')) {
    pem.value = text
    inputEncoding.value = 'text'
  } else {
    let binary = ''
    for (const byte of bytes) binary += String.fromCharCode(byte)
    pem.value = btoa(binary)
    inputEncoding.value = 'base64'
  }
  await parse()
  target.value = ''
}

async function copy(text: string) {
  try {
    await navigator.clipboard.writeText(text)
    ui.notify('success', t('tls.msg.copied'))
  } catch {
    ui.notify('error', t('tls.msg.clipboardDenied'))
  }
}

function loadExample() {
  pem.value = ''
  inputEncoding.value = 'text'
  ui.notify('info', t('tls.msg.hint'))
}
</script>

<template>
  <div class="page tls-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('tls.head.eyebrow') }}</span>
        <h2>{{ t('tls.head.title') }}</h2>
        <p>{{ t('tls.head.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <button class="button" @click="fileInput?.click()"><Icon name="upload" :size="16" />{{ t('tls.upload') }}</button>
        <input ref="fileInput" type="file" accept=".pem,.crt,.cer,.der" hidden @change="loadFile" />
      </div>
    </header>

    <div class="tls-grid">
      <section class="card tls-input">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('tls.input.eyebrow') }}</span><h3>{{ t('tls.input.title') }}</h3></div>
          <span class="chip mono">PEM / DER</span>
        </header>
        <div class="live-probe">
          <label class="field field--grow">
            <span>{{ t('tls.live.label') }}</span>
            <input v-model="liveUrl" class="mono" spellcheck="false" placeholder="https://example.com/" @keydown.enter.prevent="fetchLive" />
          </label>
          <button class="button button--primary" :disabled="liveLoading" @click="fetchLive">
            <span v-if="liveLoading" class="spinner" /><Icon v-else name="download" :size="16" />{{ liveLoading ? t('tls.live.fetching') : t('tls.live.action') }}
          </button>
        </div>
        <div v-if="liveError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ liveError }}</div>
        <div class="field-divider"><span>{{ t('tls.live.orPaste') }}</span></div>
        <textarea v-model="pem" class="code-input code-input--cert" spellcheck="false" placeholder="-----BEGIN CERTIFICATE-----&#10;…&#10;-----END CERTIFICATE-----" />
        <div class="form-actions">
          <button class="link-button" @click="loadExample">{{ t('tls.input.placeholderButton') }}</button>
          <button class="button button--primary" :disabled="parsing" @click="parse">
            <span v-if="parsing" class="spinner" /><Icon v-else name="shield" :size="16" />{{ t('tls.input.decode') }}
          </button>
        </div>
        <div v-if="error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ error }}</div>
        <div class="inline-alert inline-alert--info">
          <Icon name="lock" :size="16" />
          <span>{{ t('tls.input.note') }}</span>
        </div>
      </section>

      <section class="card tls-result">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('tls.result.eyebrow') }}</span><h3>{{ t('tls.result.title') }}</h3></div>
          <div v-if="info" class="card__tools">
            <span v-if="info.live" class="status-pill" :class="info.live.verified ? 'status-pill--ok' : 'status-pill--bad'">
              {{ info.live.verified ? t('tls.live.verified') : t('tls.live.unverified') }}
            </span>
            <span class="status-pill" :class="`status-pill--${info.expired ? 'bad' : info.daysRemaining < 30 ? 'warn' : 'ok'}`">
              {{ info.expired ? t('tls.result.expired') : t('tls.result.daysLeft', { count: info.daysRemaining }) }}
            </span>
            <button class="icon-button icon-button--sm" :title="t('tls.result.copyJson')" @click="copy(JSON.stringify(info, null, 2))"><Icon name="copy" :size="15" /></button>
          </div>
        </header>

        <div v-if="info?.live" class="inline-alert" :class="info.live.verified ? 'inline-alert--info' : 'inline-alert--danger'">
          <Icon name="shield" :size="16" />
          <span>
            {{ info.live.verified ? t('tls.live.verifiedBody') : t('tls.live.unverifiedBody', { error: info.live.verifyError }) }}
          </span>
          <span v-if="info.host" class="chip mono">{{ info.host }}:{{ info.port }}</span>
          <span v-if="info.live.alpn" class="chip mono">ALPN {{ info.live.alpn }}</span>
          <span class="chip mono">{{ t('tls.live.chainCount', { count: info.live.certificateCount ?? info.certificateCount }) }}</span>
        </div>

        <div v-if="!info" class="tls-empty">
          <div class="cert-art">
            <div class="cert-art__ring" />
            <Icon name="shield" :size="30" />
          </div>
          <h3>{{ t('tls.empty.title') }}</h3>
          <p>{{ t('tls.empty.body') }}</p>
        </div>
        <template v-else>
          <div class="cert-hero">
            <div class="cert-badge" :class="{ 'cert-badge--self': info.selfSigned }">
              <Icon name="shield" :size="22" />
            </div>
            <div class="cert-hero__copy">
              <h4>{{ info.subject }}</h4>
              <p>{{ t('tls.cert.issuedBy', { issuer: info.issuer }) }}</p>
              <div class="chip-row">
                <span class="chip">v{{ info.version }}</span>
                <span v-if="info.selfSigned" class="chip chip--warn">{{ t('tls.cert.selfSigned') }}</span>
                <span class="chip mono">{{ info.publicKey.algorithm }} {{ info.publicKey.bits }} bit</span>
              </div>
            </div>
          </div>

          <div class="validity">
            <div class="validity__labels"><span>{{ info.notBefore }}</span><span>{{ info.notAfter }}</span></div>
            <div class="validity__bar"><span :class="`validity__bar--${validityTone}`" :style="{ width: `${validityPercent}%` }" /></div>
            <div class="validity__foot">
              <span><i class="status-dot" :class="{ 'status-dot--warn': validityTone === 'warn', 'status-dot--bad': validityTone === 'bad' }" />{{ info.expired ? t('tls.validity.outside') : t('tls.validity.current') }}</span>
              <span class="mono">{{ t('tls.validity.lifetime', { percent: validityPercent.toFixed(0) }) }}</span>
            </div>
          </div>

          <div class="cert-grid">
            <div class="cert-field"><span>{{ t('tls.field.serial') }}</span><code class="mono">{{ info.serial }}</code></div>
            <div class="cert-field"><span>{{ t('tls.field.signature') }}</span><code class="mono">{{ info.signatureAlgorithm }}</code></div>
            <div class="cert-field cert-field--wide"><span>{{ t('tls.field.fingerprint') }}</span><code class="mono">{{ info.fingerprintSha256 }}</code></div>
          </div>

          <div v-if="info.certificates.length > 1" class="cert-section">
            <div class="cert-section__head">
              <span>{{ t('tls.chain.title') }}</span>
              <span class="mono">{{ info.certificateCount }}<template v-if="info.chainTruncated">+</template></span>
            </div>
            <div class="chain-list">
              <div
                v-for="cert in info.certificates"
                :key="cert.derSha256"
                class="chain-item"
                :class="{ 'chain-item--leaf': cert.leaf, 'chain-item--expired': cert.expired }"
              >
                <span class="chip mono">#{{ cert.index + 1 }}</span>
                <div class="chain-item__body">
                  <strong>{{ cert.subject }}</strong>
                  <small>{{ cert.issuer }}</small>
                </div>
                <span class="mono chain-item__days">{{ cert.daysRemaining }}d</span>
              </div>
            </div>
            <p class="muted chain-hint">{{ t('tls.chain.hint') }}</p>
          </div>

          <div class="cert-section">
            <div class="cert-section__head"><span>{{ t('tls.field.san') }}</span><span class="mono">{{ info.san.length }}</span></div>
            <div v-if="info.san.length" class="san-list">
              <span v-for="name in info.san" :key="name.type + name.value" class="san-chip">
                <small>{{ name.type }}</small>{{ name.value }}
              </span>
            </div>
            <p v-else class="muted">{{ t('tls.field.noSan') }}</p>
          </div>

          <div class="cert-columns">
            <div class="cert-section">
              <div class="cert-section__head"><span>{{ t('tls.field.keyUsage') }}</span></div>
              <div class="usage-list">
                <span v-for="usage in info.keyUsage" :key="usage" class="usage-chip">{{ usage }}</span>
                <span v-if="!info.keyUsage.length" class="muted">{{ t('tls.field.notRestricted') }}</span>
              </div>
            </div>
            <div class="cert-section">
              <div class="cert-section__head"><span>{{ t('tls.field.eku') }}</span></div>
              <div class="usage-list">
                <span v-for="usage in info.extendedKeyUsage" :key="usage" class="usage-chip">{{ usage }}</span>
                <span v-if="!info.extendedKeyUsage.length" class="muted">{{ t('tls.field.notSpecified') }}</span>
              </div>
            </div>
          </div>
          <p class="cert-note">{{ t('tls.chain.summary', { count: info.certificateCount }) }}</p>
        </template>
      </section>
    </div>
  </div>
</template>
