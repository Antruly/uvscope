<script setup lang="ts">
import { onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { errorMessage, get, post, put } from '../api'
import type { ModuleMap, UpdateInfo } from '../types'
import { useAuthStore } from '../stores/auth'
import { useI18nStore } from '../stores/i18n'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'SettingsView' })

const auth = useAuthStore()
const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
const modules = ref<ModuleMap>({ http: true, websocket: true, tcpudp: true, soap: true, tls: true, proxy: true, terminal: false })
const autoUpdateCheck = ref(true)
const updateIntervalHours = ref(24)
const dataDir = ref('')
const loading = ref(true)
const saving = ref(false)
const updateInfo = ref<UpdateInfo | null>(null)
const checkingUpdate = ref(false)

const moduleMeta: Array<{ key: keyof ModuleMap; icon: string; titleKey: string; descriptionKey: string }> = [
  { key: 'http', icon: 'bolt', titleKey: 'settings.modules.http.title', descriptionKey: 'settings.modules.http.description' },
  { key: 'websocket', icon: 'pulse', titleKey: 'settings.modules.websocket.title', descriptionKey: 'settings.modules.websocket.description' },
  { key: 'tcpudp', icon: 'layers', titleKey: 'settings.modules.tcpudp.title', descriptionKey: 'settings.modules.tcpudp.description' },
  { key: 'soap', icon: 'braces', titleKey: 'settings.modules.soap.title', descriptionKey: 'settings.modules.soap.description' },
  { key: 'tls', icon: 'shield', titleKey: 'settings.modules.tls.title', descriptionKey: 'settings.modules.tls.description' },
  { key: 'proxy', icon: 'route', titleKey: 'settings.modules.proxy.title', descriptionKey: 'settings.modules.proxy.description' },
  { key: 'terminal', icon: 'terminal', titleKey: 'settings.modules.terminal.title', descriptionKey: 'settings.modules.terminal.description' },
]

async function load() {
  loading.value = true
  try {
    const response = await get<{ modules: ModuleMap; settings: { autoUpdateCheck: boolean; updateIntervalHours: number; dataDir: string } }>('/api/settings')
    modules.value = response.modules
    autoUpdateCheck.value = response.settings.autoUpdateCheck
    updateIntervalHours.value = response.settings.updateIntervalHours
    dataDir.value = response.settings.dataDir
  } catch (err) {
    ui.toastError(err, t('settings.notify.loadFailed'))
  } finally {
    loading.value = false
  }
}

async function save() {
  saving.value = true
  try {
    await put('/api/settings', { modules: modules.value, autoUpdateCheck: autoUpdateCheck.value, updateIntervalHours: updateIntervalHours.value })
    await auth.refresh()
    ui.notify('success', t('settings.notify.saved'), t('settings.notify.savedBody'))
  } catch (err) {
    ui.toastError(err, t('settings.notify.saveFailed'))
  } finally {
    saving.value = false
  }
}

function toggleModule(key: keyof ModuleMap) {
  if (key === 'terminal' && !modules.value.terminal &&
      !window.confirm(t('settings.modules.terminal.confirm'))) {
    return
  }
  modules.value[key] = !modules.value[key]
}

async function checkUpdates(force = true) {
  checkingUpdate.value = true
  try {
    updateInfo.value = await get<UpdateInfo>(`/api/update/check${force ? '?force=1' : ''}`)
    if (!updateInfo.value.ok) ui.notify('warning', t('settings.notify.updateCheckFailed'), updateInfo.value.error)
    else if (updateInfo.value.updateAvailable) ui.notify('info', t('settings.notify.updateAvailable'), updateInfo.value.latestVersion)
    else ui.notify('success', t('settings.notify.upToDate'))
  } catch (err) {
    ui.toastError(err, t('settings.notify.updateCheckFailed'))
  } finally {
    checkingUpdate.value = false
  }
}

async function resetModule(module: string, labelKey: string) {
  const label = t(labelKey)
  if (!window.confirm(t('settings.reset.confirm', { label }))) return
  try {
    await post('/api/reset', { module })
    ui.notify('success', t('settings.reset.success', { label }))
    if (module === 'proxy') window.location.reload()
  } catch (err) {
    ui.toastError(err, t('settings.reset.failed'))
  }
}

async function factoryReset() {
  const phrase = window.prompt(t('settings.factory.prompt'))
  if (phrase !== 'RESET') return
  try {
    await post('/api/reset', { module: 'all' })
    ui.notify('success', t('settings.factory.success'), t('settings.factory.successBody'))
    window.setTimeout(() => window.location.reload(), 500)
  } catch (err) {
    ui.toastError(err, t('settings.factory.failed'))
  }
}

onMounted(load)
</script>

<template>
  <div class="page settings-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('settings.eyebrow') }}</span>
        <h2>{{ t('settings.title') }}</h2>
        <p>{{ t('settings.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <button class="button button--primary" :disabled="saving || loading" @click="save">
          <span v-if="saving" class="spinner" /><Icon v-else name="check" :size="16" />{{ t('settings.save') }}
        </button>
      </div>
    </header>

    <div v-if="loading" class="proxy-loading"><span class="spinner" />{{ t('settings.loading') }}</div>
    <div v-else class="settings-grid">
      <section class="card">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('settings.surfaces') }}</span><h3>{{ t('settings.protocolModules') }}</h3></div>
          <span class="chip">{{ t('settings.enabledCount', { count: Object.values(modules).filter(Boolean).length }) }}</span>
        </header>
        <div class="module-list">
          <div v-for="module in moduleMeta" :key="module.key" class="module-row">
            <span class="module-row__icon"><Icon :name="module.icon" :size="18" /></span>
            <div class="module-row__copy"><strong>{{ t(module.titleKey) }}</strong><p>{{ t(module.descriptionKey) }}</p></div>
            <button class="switch" :class="{ on: modules[module.key] }" :aria-pressed="modules[module.key]" @click="toggleModule(module.key)"><i /></button>
          </div>
        </div>
        <div class="inline-alert inline-alert--info"><Icon name="alert" :size="16" />{{ t('settings.disableNote') }}</div>
      </section>

      <div class="settings-stack">
        <section class="card">
          <header class="card__head"><div><span class="eyebrow">{{ t('settings.maintenance') }}</span><h3>{{ t('settings.releaseChannel') }}</h3></div><Icon name="refresh" :size="18" class="muted" /></header>
          <div class="setting-row">
            <div><strong>{{ t('settings.checkGithub') }}</strong><p>{{ t('settings.checkGithubHint') }}</p></div>
            <button class="switch" :class="{ on: autoUpdateCheck }" @click="autoUpdateCheck = !autoUpdateCheck"><i /></button>
          </div>
          <label class="field"><span>{{ t('settings.checkInterval') }}</span><input v-model.number="updateIntervalHours" type="number" min="1" max="168" /></label>
          <div class="form-actions">
            <span class="muted mono">{{ t('settings.currentVersion', { version: auth.version }) }}</span>
            <button class="button" :disabled="checkingUpdate" @click="checkUpdates(true)">
              <span v-if="checkingUpdate" class="spinner" /><Icon v-else name="refresh" :size="15" />{{ t('settings.checkNow') }}
            </button>
          </div>
          <div v-if="updateInfo" class="update-card">
            <div class="update-card__head">
              <span class="status-pill" :class="updateInfo.updateAvailable ? 'status-pill--warn' : 'status-pill--ok'">
                {{ updateInfo.updateAvailable ? t('settings.updateAvailablePill') : t('settings.upToDatePill') }}
              </span>
              <span class="mono">{{ updateInfo.latestVersion }}</span>
            </div>
            <p v-if="updateInfo.releaseName">{{ updateInfo.releaseName }}</p>
            <a v-if="updateInfo.htmlUrl" :href="updateInfo.htmlUrl" target="_blank" rel="noreferrer" class="link-button">{{ t('settings.openReleaseNotes') }} <Icon name="external" :size="13" /></a>
          </div>
        </section>

        <section class="card">
          <header class="card__head"><div><span class="eyebrow">{{ t('settings.storage') }}</span><h3>{{ t('settings.localData') }}</h3></div><Icon name="database" :size="18" class="muted" /></header>
          <div class="path-box"><span>{{ t('settings.dataDirectory') }}</span><code class="mono">{{ dataDir }}</code></div>
          <div class="path-box"><span>{{ t('settings.engine') }}</span><code class="mono">{{ t('settings.engineValue') }}</code></div>
          <p class="muted settings-note">{{ t('settings.storageNote') }}</p>
        </section>
      </div>

      <section class="card settings-danger">
        <header class="card__head"><div><span class="eyebrow">{{ t('settings.dangerZone') }}</span><h3>{{ t('settings.resetData') }}</h3></div><Icon name="alert" :size="18" /></header>
        <div class="reset-grid">
          <button class="reset-action" @click="resetModule('http', 'settings.reset.http')"><Icon name="bolt" :size="16" /><span><strong>{{ t('settings.reset.http') }}</strong><small>{{ t('settings.reset.httpHint') }}</small></span></button>
          <button class="reset-action" @click="resetModule('tcpudp', 'settings.reset.tcpudp')"><Icon name="layers" :size="16" /><span><strong>{{ t('settings.reset.tcpudp') }}</strong><small>{{ t('settings.reset.tcpudpHint') }}</small></span></button>
          <button class="reset-action" @click="resetModule('tls', 'settings.reset.tls')"><Icon name="shield" :size="16" /><span><strong>{{ t('settings.reset.tls') }}</strong><small>{{ t('settings.reset.tlsHint') }}</small></span></button>
          <button class="reset-action" @click="resetModule('proxy', 'settings.reset.proxy')"><Icon name="route" :size="16" /><span><strong>{{ t('settings.reset.proxy') }}</strong><small>{{ t('settings.reset.proxyHint') }}</small></span></button>
          <button class="reset-action" @click="resetModule('history', 'settings.reset.history')"><Icon name="clock" :size="16" /><span><strong>{{ t('settings.reset.history') }}</strong><small>{{ t('settings.reset.historyHint') }}</small></span></button>
          <button class="reset-action reset-action--danger" @click="factoryReset"><Icon name="trash" :size="16" /><span><strong>{{ t('settings.reset.factory') }}</strong><small>{{ t('settings.reset.factoryHint') }}</small></span></button>
        </div>
      </section>
    </div>
  </div>
</template>
