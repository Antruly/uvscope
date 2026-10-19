<script setup lang="ts">
import { onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { errorMessage, get, post, put } from '../api'
import type { ModuleMap, UpdateInfo } from '../types'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'SettingsView' })

const auth = useAuthStore()
const ui = useUiStore()
const modules = ref<ModuleMap>({ http: true, websocket: true, tcpudp: true, soap: true, tls: true, proxy: true })
const autoUpdateCheck = ref(true)
const updateIntervalHours = ref(24)
const dataDir = ref('')
const loading = ref(true)
const saving = ref(false)
const updateInfo = ref<UpdateInfo | null>(null)
const checkingUpdate = ref(false)

const moduleMeta: Array<{ key: keyof ModuleMap; icon: string; title: string; description: string }> = [
  { key: 'http', icon: 'bolt', title: 'HTTP Studio', description: 'Request builder, history, HTTP/1.1 and protocol preference.' },
  { key: 'websocket', icon: 'pulse', title: 'WebSocket console', description: 'Browser-native frame trace with text and binary messages.' },
  { key: 'tcpudp', icon: 'layers', title: 'TCP / UDP bench', description: 'One-shot transport probes through libuvcpp.' },
  { key: 'soap', icon: 'braces', title: 'SOAP / WSDL', description: 'WSDL operation discovery, envelope editing and fault view.' },
  { key: 'tls', icon: 'shield', title: 'TLS Inspector', description: 'Local X.509 decoding and validity analysis.' },
  { key: 'proxy', icon: 'route', title: 'Reverse proxy', description: 'Managed HTTP gateways with live request logs.' },
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
    ui.toastError(err, 'Could not load settings')
  } finally {
    loading.value = false
  }
}

async function save() {
  saving.value = true
  try {
    await put('/api/settings', { modules: modules.value, autoUpdateCheck: autoUpdateCheck.value, updateIntervalHours: updateIntervalHours.value })
    await auth.refresh()
    ui.notify('success', 'Settings saved', 'Module changes apply immediately.')
  } catch (err) {
    ui.toastError(err, 'Could not save settings')
  } finally {
    saving.value = false
  }
}

function toggleModule(key: keyof ModuleMap) {
  modules.value[key] = !modules.value[key]
}

async function checkUpdates(force = true) {
  checkingUpdate.value = true
  try {
    updateInfo.value = await get<UpdateInfo>(`/api/update/check${force ? '?force=1' : ''}`)
    if (!updateInfo.value.ok) ui.notify('warning', 'Update check failed', updateInfo.value.error)
    else if (updateInfo.value.updateAvailable) ui.notify('info', 'Update available', updateInfo.value.latestVersion)
    else ui.notify('success', 'You are up to date')
  } catch (err) {
    ui.toastError(err, 'Update check failed')
  } finally {
    checkingUpdate.value = false
  }
}

async function resetModule(module: string, label: string) {
  if (!window.confirm(`Reset ${label}? Its stored history/config is removed. Other modules are untouched.`)) return
  try {
    await post('/api/reset', { module })
    ui.notify('success', `${label} reset`)
    if (module === 'proxy') window.location.reload()
  } catch (err) {
    ui.toastError(err, 'Reset failed')
  }
}

async function factoryReset() {
  const phrase = window.prompt('This deletes the SQLite database, all users, settings, history and proxies. Type RESET to continue.')
  if (phrase !== 'RESET') return
  try {
    await post('/api/reset', { module: 'all' })
    ui.notify('success', 'Factory reset complete', 'Reloading into first-run setup.')
    window.setTimeout(() => window.location.reload(), 500)
  } catch (err) {
    ui.toastError(err, 'Factory reset failed')
  }
}

onMounted(load)
</script>

<template>
  <div class="page settings-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">Settings</span>
        <h2>Tune the workbench.</h2>
        <p>Module availability, update policy and local storage controls. Changes are admin-only.</p>
      </div>
      <div class="page-head__actions">
        <button class="button button--primary" :disabled="saving || loading" @click="save">
          <span v-if="saving" class="spinner" /><Icon v-else name="check" :size="16" />Save changes
        </button>
      </div>
    </header>

    <div v-if="loading" class="proxy-loading"><span class="spinner" />Loading settings…</div>
    <div v-else class="settings-grid">
      <section class="card">
        <header class="card__head">
          <div><span class="eyebrow">Surfaces</span><h3>Protocol modules</h3></div>
          <span class="chip">{{ Object.values(modules).filter(Boolean).length }} enabled</span>
        </header>
        <div class="module-list">
          <div v-for="module in moduleMeta" :key="module.key" class="module-row">
            <span class="module-row__icon"><Icon :name="module.icon" :size="18" /></span>
            <div class="module-row__copy"><strong>{{ module.title }}</strong><p>{{ module.description }}</p></div>
            <button class="switch" :class="{ on: modules[module.key] }" :aria-pressed="modules[module.key]" @click="toggleModule(module.key)"><i /></button>
          </div>
        </div>
        <div class="inline-alert inline-alert--info"><Icon name="alert" :size="16" />Disabling a module hides it from navigation and rejects its API calls. Stored history remains.</div>
      </section>

      <div class="settings-stack">
        <section class="card">
          <header class="card__head"><div><span class="eyebrow">Maintenance</span><h3>Release channel</h3></div><Icon name="refresh" :size="18" class="muted" /></header>
          <div class="setting-row">
            <div><strong>Check GitHub for updates</strong><p>Uses the public Releases API; no account required.</p></div>
            <button class="switch" :class="{ on: autoUpdateCheck }" @click="autoUpdateCheck = !autoUpdateCheck"><i /></button>
          </div>
          <label class="field"><span>Check interval (hours)</span><input v-model.number="updateIntervalHours" type="number" min="1" max="168" /></label>
          <div class="form-actions">
            <span class="muted mono">current v{{ auth.version }}</span>
            <button class="button" :disabled="checkingUpdate" @click="checkUpdates(true)">
              <span v-if="checkingUpdate" class="spinner" /><Icon v-else name="refresh" :size="15" />Check now
            </button>
          </div>
          <div v-if="updateInfo" class="update-card">
            <div class="update-card__head">
              <span class="status-pill" :class="updateInfo.updateAvailable ? 'status-pill--warn' : 'status-pill--ok'">
                {{ updateInfo.updateAvailable ? 'UPDATE AVAILABLE' : 'UP TO DATE' }}
              </span>
              <span class="mono">{{ updateInfo.latestVersion }}</span>
            </div>
            <p v-if="updateInfo.releaseName">{{ updateInfo.releaseName }}</p>
            <a v-if="updateInfo.htmlUrl" :href="updateInfo.htmlUrl" target="_blank" rel="noreferrer" class="link-button">Open release notes <Icon name="external" :size="13" /></a>
          </div>
        </section>

        <section class="card">
          <header class="card__head"><div><span class="eyebrow">Storage</span><h3>Local data</h3></div><Icon name="database" :size="18" class="muted" /></header>
          <div class="path-box"><span>Data directory</span><code class="mono">{{ dataDir }}</code></div>
          <div class="path-box"><span>Engine</span><code class="mono">SQLite · WAL · local-only</code></div>
          <p class="muted settings-note">Deleting the SQLite database while uvscope is stopped also triggers first-run setup.</p>
        </section>
      </div>

      <section class="card settings-danger">
        <header class="card__head"><div><span class="eyebrow">Danger zone</span><h3>Reset data</h3></div><Icon name="alert" :size="18" /></header>
        <div class="reset-grid">
          <button class="reset-action" @click="resetModule('http', 'HTTP history')"><Icon name="bolt" :size="16" /><span><strong>HTTP history</strong><small>Request summaries</small></span></button>
          <button class="reset-action" @click="resetModule('tcpudp', 'TCP/UDP history')"><Icon name="layers" :size="16" /><span><strong>TCP / UDP history</strong><small>Transport probes</small></span></button>
          <button class="reset-action" @click="resetModule('tls', 'TLS history')"><Icon name="shield" :size="16" /><span><strong>TLS history</strong><small>Certificate decodes</small></span></button>
          <button class="reset-action" @click="resetModule('proxy', 'proxy configuration')"><Icon name="route" :size="16" /><span><strong>Proxy config</strong><small>Listeners and logs</small></span></button>
          <button class="reset-action" @click="resetModule('history', 'all history')"><Icon name="clock" :size="16" /><span><strong>All history</strong><small>Keep users/settings</small></span></button>
          <button class="reset-action reset-action--danger" @click="factoryReset"><Icon name="trash" :size="16" /><span><strong>Factory reset</strong><small>Delete the database</small></span></button>
        </div>
      </section>
    </div>
  </div>
</template>
