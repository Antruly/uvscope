<script setup lang="ts">
import { computed, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { errorMessage, post } from '../api'
import type { CertificateInfo } from '../types'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'TlsView' })

const ui = useUiStore()
const pem = ref('')
const info = ref<CertificateInfo | null>(null)
const error = ref('')
const parsing = ref(false)
const fileInput = ref<HTMLInputElement | null>(null)
const inputEncoding = ref<'text' | 'base64'>('text')

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
    ui.notify('warning', 'Paste a PEM or DER certificate first')
    return
  }
  parsing.value = true
  error.value = ''
  info.value = null
  if (pem.value.includes('-----BEGIN CERTIFICATE-----')) inputEncoding.value = 'text'
  try {
    info.value = await post<CertificateInfo>('/api/tls/inspect', { pem: pem.value, encoding: inputEncoding.value })
    ui.notify('success', 'Certificate decoded')
  } catch (err) {
    error.value = errorMessage(err)
    ui.toastError(err, 'Certificate could not be parsed')
  } finally {
    parsing.value = false
  }
}

async function loadFile(event: Event) {
  const target = event.target as HTMLInputElement
  const file = target.files?.[0]
  if (!file) return
  if (file.size > 1024 * 1024) {
    ui.notify('warning', 'Certificate file is larger than 1 MiB')
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
    ui.notify('success', 'Copied')
  } catch {
    ui.notify('error', 'Clipboard access denied')
  }
}

function loadExample() {
  pem.value = ''
  inputEncoding.value = 'text'
  ui.notify('info', 'Paste PEM text or upload a PEM/DER file to decode it')
}
</script>

<template>
  <div class="page tls-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">TLS Inspector</span>
        <h2>Know what the handshake trusts.</h2>
        <p>Decode a PEM certificate or DER file locally. Nothing is uploaded to a third party.</p>
      </div>
      <div class="page-head__actions">
        <button class="button" @click="fileInput?.click()"><Icon name="upload" :size="16" />Upload certificate</button>
        <input ref="fileInput" type="file" accept=".pem,.crt,.cer,.der" hidden @change="loadFile" />
      </div>
    </header>

    <div class="tls-grid">
      <section class="card tls-input">
        <header class="card__head">
          <div><span class="eyebrow">Input</span><h3>Certificate material</h3></div>
          <span class="chip mono">PEM / DER</span>
        </header>
        <textarea v-model="pem" class="code-input code-input--cert" spellcheck="false" placeholder="-----BEGIN CERTIFICATE-----&#10;…&#10;-----END CERTIFICATE-----" />
        <div class="form-actions">
          <button class="link-button" @click="loadExample">Paste a placeholder</button>
          <button class="button button--primary" :disabled="parsing" @click="parse">
            <span v-if="parsing" class="spinner" /><Icon v-else name="shield" :size="16" />Decode certificate
          </button>
        </div>
        <div v-if="error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ error }}</div>
        <div class="inline-alert inline-alert--info">
          <Icon name="lock" :size="16" />
          <span>Chain parsing shows the leaf first in 0.1.0. Live TLS handshake inspection is planned next.</span>
        </div>
      </section>

      <section class="card tls-result">
        <header class="card__head">
          <div><span class="eyebrow">Decoded</span><h3>Certificate profile</h3></div>
          <div v-if="info" class="card__tools">
            <span class="status-pill" :class="`status-pill--${info.expired ? 'bad' : info.daysRemaining < 30 ? 'warn' : 'ok'}`">
              {{ info.expired ? 'EXPIRED' : `${info.daysRemaining} days left` }}
            </span>
            <button class="icon-button icon-button--sm" title="Copy JSON" @click="copy(JSON.stringify(info, null, 2))"><Icon name="copy" :size="15" /></button>
          </div>
        </header>

        <div v-if="!info" class="tls-empty">
          <div class="cert-art">
            <div class="cert-art__ring" />
            <Icon name="shield" :size="30" />
          </div>
          <h3>No certificate decoded</h3>
          <p>Upload a certificate or paste PEM text to inspect subject, issuer, SANs, key usage and validity.</p>
        </div>
        <template v-else>
          <div class="cert-hero">
            <div class="cert-badge" :class="{ 'cert-badge--self': info.selfSigned }">
              <Icon name="shield" :size="22" />
            </div>
            <div class="cert-hero__copy">
              <h4>{{ info.subject }}</h4>
              <p>Issued by {{ info.issuer }}</p>
              <div class="chip-row">
                <span class="chip">v{{ info.version }}</span>
                <span v-if="info.selfSigned" class="chip chip--warn">self-signed</span>
                <span class="chip mono">{{ info.publicKey.algorithm }} {{ info.publicKey.bits }} bit</span>
              </div>
            </div>
          </div>

          <div class="validity">
            <div class="validity__labels"><span>{{ info.notBefore }}</span><span>{{ info.notAfter }}</span></div>
            <div class="validity__bar"><span :class="`validity__bar--${validityTone}`" :style="{ width: `${validityPercent}%` }" /></div>
            <div class="validity__foot">
              <span><i class="status-dot" :class="{ 'status-dot--warn': validityTone === 'warn', 'status-dot--bad': validityTone === 'bad' }" />{{ info.expired ? 'outside validity window' : 'currently valid' }}</span>
              <span class="mono">{{ validityPercent.toFixed(0) }}% of lifetime</span>
            </div>
          </div>

          <div class="cert-grid">
            <div class="cert-field"><span>Serial</span><code class="mono">{{ info.serial }}</code></div>
            <div class="cert-field"><span>Signature</span><code class="mono">{{ info.signatureAlgorithm }}</code></div>
            <div class="cert-field cert-field--wide"><span>SHA-256 fingerprint</span><code class="mono">{{ info.fingerprintSha256 }}</code></div>
          </div>

          <div class="cert-section">
            <div class="cert-section__head"><span>Subject alternative names</span><span class="mono">{{ info.san.length }}</span></div>
            <div v-if="info.san.length" class="san-list">
              <span v-for="name in info.san" :key="name.type + name.value" class="san-chip">
                <small>{{ name.type }}</small>{{ name.value }}
              </span>
            </div>
            <p v-else class="muted">No subject alternative names.</p>
          </div>

          <div class="cert-columns">
            <div class="cert-section">
              <div class="cert-section__head"><span>Key usage</span></div>
              <div class="usage-list">
                <span v-for="usage in info.keyUsage" :key="usage" class="usage-chip">{{ usage }}</span>
                <span v-if="!info.keyUsage.length" class="muted">Not restricted</span>
              </div>
            </div>
            <div class="cert-section">
              <div class="cert-section__head"><span>Extended usage</span></div>
              <div class="usage-list">
                <span v-for="usage in info.extendedKeyUsage" :key="usage" class="usage-chip">{{ usage }}</span>
                <span v-if="!info.extendedKeyUsage.length" class="muted">Not specified</span>
              </div>
            </div>
          </div>
          <p class="cert-note">{{ info.chainNotice }}</p>
        </template>
      </section>
    </div>
  </div>
</template>
