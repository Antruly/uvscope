<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { del, errorMessage, get, post } from '../api'
import type { CertificateInfo } from '../types'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'

defineOptions({ name: 'TlsView' })

const ui = useUiStore()
const auth = useAuthStore()
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

interface ManagedCertificate {
  id: string
  name: string
  source: string
  createdAt: string
  keyType: string
  keyBits: number
  subject: string
  issuer: string
  notAfter: string
  expired: boolean
  certificateCount: number
  certPath: string
  keyPath: string
  renewable?: boolean
  renewCount?: number
  renewedAt?: string
  caId?: string
  caSubject?: string
}

interface LocalCa {
  ok: boolean
  exists: boolean
  name?: string
  subject?: string
  notBefore?: string
  notAfter?: string
  daysRemaining?: number
  expired?: boolean
  createdAt?: string
  fingerprintSha256?: string
  certPath?: string
  publicKey?: { algorithm: string; bits: number }
}

const certificates = ref<ManagedCertificate[]>([])
const certLoading = ref(false)
const certError = ref('')
const generateOpen = ref(false)
const importOpen = ref(false)
const certBusy = ref(false)
const genForm = ref({
  name: '',
  commonName: 'localhost',
  sans: 'localhost, 127.0.0.1',
  days: 365,
  keyType: 'rsa' as 'rsa' | 'ec',
  keyBits: 2048,
  signedBy: 'self' as 'self' | 'ca',
})
const ca = ref<LocalCa | null>(null)
const caLoading = ref(false)
const caError = ref('')
const caBusy = ref(false)
const caOpen = ref(false)
const caForm = ref({ commonName: 'uvscope Local CA', days: 3650 })
const importForm = ref({ name: '', certPem: '', keyPem: '', password: '' })
const certFile = ref<HTMLInputElement | null>(null)
const keyFile = ref<HTMLInputElement | null>(null)
const canManage = computed(() => auth.user?.role === 'admin')

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

async function loadCertificates() {
  certLoading.value = true
  certError.value = ''
  try {
    const response = await get<{ ok: boolean; certificates: ManagedCertificate[] }>(
      '/api/tls/certificates',
    )
    if (response.ok) certificates.value = response.certificates
  } catch (err) {
    certError.value = errorMessage(err)
  } finally {
    certLoading.value = false
  }
}

async function loadCa() {
  caLoading.value = true
  caError.value = ''
  try {
    ca.value = await get<LocalCa>('/api/tls/ca')
    if (ca.value.exists && ca.value.expired) {
      caError.value = t('tls.ca.expiredWarning')
    }
  } catch (err) {
    caError.value = errorMessage(err)
  } finally {
    caLoading.value = false
  }
}

async function createCa() {
  caBusy.value = true
  caError.value = ''
  try {
    const response = await post<{ ok: boolean; ca?: LocalCa; error?: string }>(
      '/api/tls/ca',
      caForm.value,
    )
    if (!response.ok || !response.ca) {
      caError.value = response.error || t('tls.ca.createFailed')
      return
    }
    caOpen.value = false
    ca.value = response.ca
    ui.notify('success', t('tls.ca.created'), response.ca.subject)
  } catch (err) {
    caError.value = errorMessage(err)
  } finally {
    caBusy.value = false
  }
}

function downloadCa() {
  window.open('/api/tls/ca/cert', '_blank', 'noopener')
}

function openIssue() {
  genForm.value.signedBy = 'ca'
  generateOpen.value = true
  certError.value = ''
}

function canRenew(cert: ManagedCertificate) {
  if (cert.renewable === true) return true
  return cert.renewable === undefined && (cert.source === 'generated' || cert.source === 'ca')
}

async function renewCertificate(cert: ManagedCertificate) {
  if (!window.confirm(t('tls.vault.renewConfirm', { name: cert.name }))) return
  try {
    const response = await post<{
      ok: boolean
      certificate?: ManagedCertificate
      error?: string
    }>(`/api/tls/certificates/${cert.id}/renew`, { days: 365 })
    if (!response.ok || !response.certificate) {
      ui.notify('error', t('tls.vault.renewFailed'), response.error)
      return
    }
    const index = certificates.value.findIndex((item) => item.id === cert.id)
    if (index >= 0) certificates.value[index] = response.certificate
    ui.notify('success', t('tls.vault.renewed'), response.certificate.notAfter)
  } catch (err) {
    ui.toastError(err, t('tls.vault.renewFailed'))
  }
}

function splitSans(value: string) {
  return value
    .split(/[\n,]+/)
    .map((item) => item.trim())
    .filter(Boolean)
}

async function generateCertificate() {
  certBusy.value = true
  certError.value = ''
  try {
    const response = await post<{ ok: boolean; certificate?: ManagedCertificate; error?: string }>(
      '/api/tls/certificates/generate',
      {
        name: genForm.value.name,
        commonName: genForm.value.commonName,
        sans: splitSans(genForm.value.sans),
        days: genForm.value.days,
        keyType: genForm.value.keyType,
        keyBits: genForm.value.keyBits,
        signedBy: genForm.value.signedBy,
      },
    )
    if (!response.ok || !response.certificate) {
      certError.value = response.error || t('tls.vault.generateFailed')
      return
    }
    generateOpen.value = false
    await loadCertificates()
    ui.notify('success', t('tls.vault.generated'), response.certificate.subject)
  } catch (err) {
    certError.value = errorMessage(err)
  } finally {
    certBusy.value = false
  }
}

async function importCertificate() {
  certBusy.value = true
  certError.value = ''
  try {
    const response = await post<{ ok: boolean; certificate?: ManagedCertificate; error?: string }>(
      '/api/tls/certificates/import',
      importForm.value,
    )
    if (!response.ok || !response.certificate) {
      certError.value = response.error || t('tls.vault.importFailed')
      return
    }
    importOpen.value = false
    importForm.value = { name: '', certPem: '', keyPem: '', password: '' }
    await loadCertificates()
    ui.notify('success', t('tls.vault.imported'), response.certificate.subject)
  } catch (err) {
    certError.value = errorMessage(err)
  } finally {
    certBusy.value = false
  }
}

async function deleteCertificate(cert: ManagedCertificate) {
  if (!window.confirm(t('tls.vault.deleteConfirm', { name: cert.name }))) return
  try {
    const response = await del<{ ok: boolean; error?: string }>(
      `/api/tls/certificates/${cert.id}`,
    )
    if (!response.ok) {
      ui.notify('error', t('tls.vault.deleteFailed'), response.error)
      return
    }
    certificates.value = certificates.value.filter((item) => item.id !== cert.id)
    ui.notify('success', t('tls.vault.deleted'))
  } catch (err) {
    ui.toastError(err, t('tls.vault.deleteFailed'))
  }
}

async function inspectManaged(cert: ManagedCertificate) {
  try {
    const response = await get<{ ok: boolean; certificate: { certificate?: CertificateInfo } }>(
      `/api/tls/certificates/${cert.id}`,
    )
    const parsed = response.certificate?.certificate
    if (parsed) {
      info.value = { ...parsed, ok: true }
      liveError.value = ''
      ui.notify('success', t('tls.msg.decoded'), cert.name)
    }
  } catch (err) {
    ui.toastError(err, t('tls.msg.parseFailed'))
  }
}

async function readTextFile(event: Event, target: 'cert' | 'key') {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file) return
  if (file.size > 1024 * 1024) {
    ui.notify('warning', t('tls.msg.fileTooLarge'))
    return
  }
  const text = await file.text()
  if (target === 'cert') importForm.value.certPem = text
  else importForm.value.keyPem = text
  input.value = ''
}

function downloadCertificate(cert: ManagedCertificate) {
  window.open(`/api/tls/certificates/${cert.id}/cert`, '_blank', 'noopener')
}

onMounted(() => {
  void loadCertificates()
  void loadCa()
})
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

    <section class="card cert-vault tls-ca">
      <header class="card__head">
        <div>
          <span class="eyebrow">{{ t('tls.ca.eyebrow') }}</span>
          <h3>{{ t('tls.ca.title') }}</h3>
        </div>
        <div v-if="canManage" class="card__tools">
          <button v-if="!ca?.exists" class="button button--sm" @click="caOpen = true; caError = ''">
            <Icon name="plus" :size="14" />{{ t('tls.ca.create') }}
          </button>
          <template v-else>
            <button class="button button--sm" @click="downloadCa()">
              <Icon name="download" :size="14" />{{ t('tls.ca.download') }}
            </button>
            <button class="button button--sm" :disabled="ca?.expired" @click="openIssue">
              <Icon name="plus" :size="14" />{{ t('tls.ca.issue') }}
            </button>
          </template>
        </div>
      </header>
      <p class="muted cert-vault__lead">{{ t('tls.ca.lead') }}</p>
      <div v-if="caError" class="inline-alert inline-alert--danger">
        <Icon name="alert" :size="16" />{{ caError }}
      </div>
      <div v-if="caLoading" class="session-empty"><span class="spinner" /></div>
      <div v-else-if="ca?.exists" class="vault-item">
        <div class="vault-main">
          <strong>{{ ca.name || ca.subject }}</strong>
          <small>
            {{ ca.subject }} ·
            {{ ca.expired ? t('tls.ca.expired') : t('tls.result.daysLeft', { count: ca.daysRemaining ?? 0 }) }}
            <template v-if="ca.notAfter"> · {{ ca.notAfter }}</template>
          </small>
          <code class="mono">{{ ca.fingerprintSha256 }}</code>
        </div>
        <div class="vault-actions">
          <span class="status-pill" :class="ca.expired ? 'status-pill--bad' : 'status-pill--ok'">
            {{ ca.expired ? t('tls.ca.expired') : t('tls.ca.valid') }}
          </span>
        </div>
      </div>
      <div v-else class="session-empty">
        <Icon name="lock" :size="20" />
        <span>{{ t('tls.ca.empty') }}</span>
      </div>
    </section>

    <section class="card cert-vault">
      <header class="card__head">
        <div>
          <span class="eyebrow">{{ t('tls.vault.eyebrow') }}</span>
          <h3>{{ t('tls.vault.title') }}</h3>
        </div>
        <div v-if="canManage" class="card__tools">
          <button class="button button--sm" @click="generateOpen = true; certError = ''">
            <Icon name="plus" :size="14" />{{ t('tls.vault.generate') }}
          </button>
          <button class="button button--sm" @click="importOpen = true; certError = ''">
            <Icon name="upload" :size="14" />{{ t('tls.vault.import') }}
          </button>
        </div>
      </header>
      <p class="muted cert-vault__lead">{{ t('tls.vault.lead') }}</p>
      <div v-if="certError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ certError }}</div>
      <div v-if="certLoading" class="session-empty"><span class="spinner" /></div>
      <div v-else-if="certificates.length" class="vault-list">
        <div v-for="cert in certificates" :key="cert.id" class="vault-item">
          <div class="vault-main">
            <strong>{{ cert.name }}</strong>
            <small>{{ cert.subject }} · {{ cert.source }} · {{ cert.notAfter }}</small>
            <code class="mono">{{ cert.certPath }}</code>
          </div>
          <div class="vault-actions">
            <button class="icon-button icon-button--sm" :title="t('tls.vault.inspect')" @click="inspectManaged(cert)">
              <Icon name="shield" :size="14" />
            </button>
            <button class="icon-button icon-button--sm" :title="t('tls.vault.download')" @click="downloadCertificate(cert)">
              <Icon name="download" :size="14" />
            </button>
            <button
              v-if="canManage && canRenew(cert)"
              class="icon-button icon-button--sm"
              :title="t('tls.vault.renew')"
              @click="renewCertificate(cert)"
            >
              <Icon name="refresh" :size="14" />
            </button>
            <button v-if="canManage" class="icon-button icon-button--sm icon-button--danger" :title="t('tls.vault.delete')" @click="deleteCertificate(cert)">
              <Icon name="trash" :size="14" />
            </button>
          </div>
        </div>
      </div>
      <div v-else class="session-empty">
        <Icon name="shield" :size="20" />
        <span>{{ t('tls.vault.empty') }}</span>
      </div>
    </section>

    <Teleport to="body">
      <Transition name="modal">
        <div v-if="generateOpen" class="modal-scrim" @click.self="generateOpen = false">
          <form class="modal-card modal-card--wide" @submit.prevent="generateCertificate">
            <header class="modal-head">
              <div><span class="eyebrow">{{ t('tls.vault.eyebrow') }}</span><h3>{{ genForm.signedBy === 'ca' ? t('tls.generate.titleCa') : t('tls.generate.title') }}</h3></div>
              <button type="button" class="icon-button" @click="generateOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <div class="form-grid">
                <label class="field field--grow"><span>{{ t('tls.generate.name') }}</span><input v-model="genForm.name" :placeholder="t('tls.generate.namePlaceholder')" /></label>
                <label class="field field--grow"><span>{{ t('tls.generate.commonName') }}</span><input v-model="genForm.commonName" class="mono" /></label>
              </div>
              <label class="field"><span>{{ t('tls.generate.sans') }}</span><textarea v-model="genForm.sans" class="code-input code-input--short" spellcheck="false" :placeholder="t('tls.generate.sansPlaceholder')" /></label>
              <div class="form-grid">
                <label class="field">
                  <span>{{ t('tls.generate.signedBy') }}</span>
                  <select v-model="genForm.signedBy" class="select">
                    <option value="self">{{ t('tls.generate.signedBySelf') }}</option>
                    <option value="ca" :disabled="!ca?.exists">{{ t('tls.generate.signedByCa') }}</option>
                  </select>
                  <small v-if="!ca?.exists" class="muted">{{ t('tls.generate.caMissing') }}</small>
                </label>
                <label class="field"><span>{{ t('tls.generate.days') }}</span><input v-model.number="genForm.days" type="number" min="1" max="3650" class="mono" /></label>
                <label class="field"><span>{{ t('tls.generate.keyType') }}</span>
                  <select v-model="genForm.keyType" class="select" @change="genForm.keyBits = genForm.keyType === 'ec' ? 256 : 2048">
                    <option value="rsa">RSA</option>
                    <option value="ec">ECDSA</option>
                  </select>
                </label>
                <label class="field"><span>{{ t('tls.generate.keyBits') }}</span>
                  <select v-model.number="genForm.keyBits" class="select">
                    <template v-if="genForm.keyType === 'rsa'">
                      <option :value="2048">2048</option>
                      <option :value="3072">3072</option>
                      <option :value="4096">4096</option>
                    </template>
                    <template v-else>
                      <option :value="256">P-256</option>
                      <option :value="384">P-384</option>
                    </template>
                  </select>
                </label>
              </div>
              <div v-if="certError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ certError }}</div>
              <p class="modal-hint">{{ genForm.signedBy === 'ca' ? t('tls.generate.caHint') : t('tls.generate.hint') }}</p>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="generateOpen = false">{{ t('tls.generate.cancel') }}</button>
              <button type="submit" class="button button--primary" :disabled="certBusy">
                <span v-if="certBusy" class="spinner" /><Icon v-else name="plus" :size="15" />{{ t('tls.generate.action') }}
              </button>
            </footer>
          </form>
        </div>
      </Transition>
    </Teleport>

    <Teleport to="body">
      <Transition name="modal">
        <div v-if="caOpen" class="modal-scrim" @click.self="caOpen = false">
          <form class="modal-card" @submit.prevent="createCa">
            <header class="modal-head">
              <div><span class="eyebrow">{{ t('tls.ca.eyebrow') }}</span><h3>{{ t('tls.ca.createTitle') }}</h3></div>
              <button type="button" class="icon-button" @click="caOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <label class="field"><span>{{ t('tls.ca.commonName') }}</span><input v-model="caForm.commonName" class="mono" /></label>
              <label class="field"><span>{{ t('tls.ca.days') }}</span><input v-model.number="caForm.days" type="number" min="1" max="7300" class="mono" /></label>
              <div v-if="caError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ caError }}</div>
              <p class="modal-hint">{{ t('tls.ca.createHint') }}</p>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="caOpen = false">{{ t('tls.generate.cancel') }}</button>
              <button type="submit" class="button button--primary" :disabled="caBusy">
                <span v-if="caBusy" class="spinner" /><Icon v-else name="lock" :size="15" />{{ t('tls.ca.createAction') }}
              </button>
            </footer>
          </form>
        </div>
      </Transition>
    </Teleport>

    <Teleport to="body">
      <Transition name="modal">
        <div v-if="importOpen" class="modal-scrim" @click.self="importOpen = false">
          <form class="modal-card modal-card--wide" @submit.prevent="importCertificate">
            <header class="modal-head">
              <div><span class="eyebrow">{{ t('tls.vault.eyebrow') }}</span><h3>{{ t('tls.import.title') }}</h3></div>
              <button type="button" class="icon-button" @click="importOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <label class="field"><span>{{ t('tls.import.name') }}</span><input v-model="importForm.name" :placeholder="t('tls.import.namePlaceholder')" /></label>
              <label class="field">
                <span>{{ t('tls.import.cert') }}</span>
                <textarea v-model="importForm.certPem" class="code-input code-input--short" spellcheck="false" placeholder="-----BEGIN CERTIFICATE-----" />
                <button type="button" class="link-button" @click="certFile?.click()"><Icon name="upload" :size="13" />{{ t('tls.import.chooseFile') }}</button>
                <input ref="certFile" type="file" accept=".pem,.crt,.cer" hidden @change="readTextFile($event, 'cert')" />
              </label>
              <label class="field">
                <span>{{ t('tls.import.key') }}</span>
                <textarea v-model="importForm.keyPem" class="code-input code-input--short" spellcheck="false" placeholder="-----BEGIN PRIVATE KEY-----" />
                <button type="button" class="link-button" @click="keyFile?.click()"><Icon name="upload" :size="13" />{{ t('tls.import.chooseFile') }}</button>
                <input ref="keyFile" type="file" accept=".pem,.key" hidden @change="readTextFile($event, 'key')" />
              </label>
              <label class="field"><span>{{ t('tls.import.password') }}</span><input v-model="importForm.password" type="password" class="mono" :placeholder="t('tls.import.passwordPlaceholder')" /></label>
              <div v-if="certError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ certError }}</div>
              <p class="modal-hint">{{ t('tls.import.hint') }}</p>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="importOpen = false">{{ t('tls.import.cancel') }}</button>
              <button type="submit" class="button button--primary" :disabled="certBusy">
                <span v-if="certBusy" class="spinner" /><Icon v-else name="upload" :size="15" />{{ t('tls.import.action') }}
              </button>
            </footer>
          </form>
        </div>
      </Transition>
    </Teleport>
  </div>
</template>
