<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import BrandMark from '../components/BrandMark.vue'
import Icon from '../components/Icon.vue'
import { errorMessage, get } from '../api'
import type { UpdateInfo } from '../types'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'AboutView' })

interface About {
  name: string
  version: string
  libuvcpp: string
  libuv: string
  platform: string
  license: string
  repository: string
  issues: string
  starCount: number
  description: string
  modules: Record<string, boolean>
  capabilities: string[]
}

const auth = useAuthStore()
const ui = useUiStore()
const about = ref<About | null>(null)
const update = ref<UpdateInfo | null>(null)
const checking = ref(false)
const loading = ref(true)
const error = ref('')

const stars = computed(() => update.value?.starCount ?? about.value?.starCount ?? 0)

async function load() {
  loading.value = true
  try {
    about.value = await get<About>('/api/about')
  } catch (err) {
    error.value = errorMessage(err)
    ui.toastError(err, 'Could not load about page')
  } finally {
    loading.value = false
  }
}

async function checkUpdates() {
  checking.value = true
  try {
    update.value = await get<UpdateInfo>('/api/update/check?force=1')
    if (!update.value.ok) ui.notify('warning', 'Update check failed', update.value.error)
    else if (update.value.updateAvailable) ui.notify('info', 'A newer release is available', update.value.latestVersion)
    else ui.notify('success', 'You are on the latest release')
  } catch (err) {
    ui.toastError(err, 'Update check failed')
  } finally {
    checking.value = false
  }
}

onMounted(load)
</script>

<template>
  <div class="page about-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">About</span>
        <h2>Built for the space between bytes.</h2>
        <p>uvscope is an open-source, local-first workbench for web protocols and gateway traffic.</p>
      </div>
      <div class="page-head__actions">
        <a class="button" href="https://github.com/Antruly/uvscope" target="_blank" rel="noreferrer">
          <Icon name="star" :size="16" />Star on GitHub
        </a>
      </div>
    </header>

    <section class="about-hero">
      <div class="about-hero__mark"><BrandMark :size="72" /></div>
      <div class="about-hero__copy">
        <span class="eyebrow">uvscope · v{{ about?.version || auth.version }}</span>
        <h1>One local scope for every handshake.</h1>
        <p>{{ about?.description || 'A libuvcpp-powered multi-protocol web debugging workbench and gateway.' }}</p>
        <div class="chip-row">
          <span class="chip chip--accent"><Icon name="spark" :size="12" />MIT licensed</span>
          <span class="chip mono">libuvcpp {{ about?.libuvcpp || auth.libuvcpp }}</span>
          <span class="chip mono">libuv {{ about?.libuv }}</span>
          <span class="chip">{{ about?.platform }}</span>
        </div>
      </div>
      <div class="about-stars">
        <Icon name="star" :size="19" />
        <strong>{{ stars }}</strong>
        <span>GitHub stars</span>
      </div>
    </section>

    <div v-if="error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ error }}</div>

    <section class="about-grid">
      <article class="card about-update">
        <header class="card__head">
          <div><span class="eyebrow">Release channel</span><h3>Version & updates</h3></div>
          <button class="button button--sm" :disabled="checking" @click="checkUpdates">
            <span v-if="checking" class="spinner" /><Icon v-else name="refresh" :size="14" />Check
          </button>
        </header>
        <div class="version-line">
          <div class="version-badge"><span>CURRENT</span><strong>v{{ about?.version || auth.version }}</strong></div>
          <div class="version-arrow"><Icon name="arrow-right" :size="17" /></div>
          <div class="version-badge version-badge--next" :class="{ 'version-badge--available': update?.updateAvailable }">
            <span>LATEST</span><strong>{{ update?.latestVersion || 'not checked' }}</strong>
          </div>
        </div>
        <div v-if="update" class="update-body">
          <div class="update-head">
            <span class="status-pill" :class="update.updateAvailable ? 'status-pill--warn' : 'status-pill--ok'">
              {{ update.updateAvailable ? 'UPDATE AVAILABLE' : 'UP TO DATE' }}
            </span>
            <span class="muted mono">{{ update.checkedAt }}</span>
          </div>
          <p v-if="update.error" class="inline-alert inline-alert--danger">{{ update.error }}</p>
          <h4 v-if="update.releaseName">{{ update.releaseName }}</h4>
          <pre v-if="update.releaseNotes" class="release-notes">{{ update.releaseNotes }}</pre>
          <a v-if="update.htmlUrl" :href="update.htmlUrl" target="_blank" rel="noreferrer" class="button">
            <Icon name="external" :size="15" />Open release notes
          </a>
        </div>
        <div v-else class="about-update__empty">
          <Icon name="download" :size="22" />
          <p>Check the public GitHub Releases feed for a newer build. No telemetry is sent.</p>
        </div>
      </article>

      <article class="card">
        <header class="card__head"><div><span class="eyebrow">Stack</span><h3>What is inside</h3></div><Icon name="layers" :size="18" class="muted" /></header>
        <div class="stack-list">
          <div class="stack-row"><span class="stack-row__badge">C++</span><div><strong>libuvcpp 1.8.0</strong><p>Event loop, HTTP, WebSocket, QUIC and SQLite bindings.</p></div></div>
          <div class="stack-row"><span class="stack-row__badge">Vue</span><div><strong>Vue 3 + Vite</strong><p>Multi-tab UI with cached workspaces and live telemetry.</p></div></div>
          <div class="stack-row"><span class="stack-row__badge">SQL</span><div><strong>SQLite WAL</strong><p>Users, sessions, settings, history and proxy configuration.</p></div></div>
          <div class="stack-row"><span class="stack-row__badge">TLS</span><div><strong>OpenSSL</strong><p>Certificate decoding, HTTPS and gateway listeners.</p></div></div>
        </div>
      </article>

      <article class="card about-capabilities">
        <header class="card__head"><div><span class="eyebrow">Surface</span><h3>Capabilities</h3></div></header>
        <div class="capability-list">
          <span v-for="capability in about?.capabilities || []" :key="capability" class="capability-chip"><Icon name="check" :size="13" />{{ capability }}</span>
        </div>
        <div class="about-links">
          <a :href="about?.repository || 'https://github.com/Antruly/uvscope'" target="_blank" rel="noreferrer"><Icon name="globe" :size="15" />Repository</a>
          <a :href="about?.issues || 'https://github.com/Antruly/uvscope/issues'" target="_blank" rel="noreferrer"><Icon name="alert" :size="15" />Issues</a>
          <a href="https://github.com/Antruly/libuvcpp" target="_blank" rel="noreferrer"><Icon name="layers" :size="15" />libuvcpp</a>
        </div>
      </article>

      <article class="card about-cta">
        <div class="about-cta__glow" />
        <Icon name="star" :size="28" />
        <h3>If uvscope saves you a debugging session, star the repo.</h3>
        <p>Stars help other protocol wranglers find the project and keep the roadmap moving.</p>
        <a class="button button--primary" href="https://github.com/Antruly/uvscope" target="_blank" rel="noreferrer">
          <Icon name="star" :size="16" />Star uvscope on GitHub
        </a>
      </article>
    </section>
  </div>
</template>
