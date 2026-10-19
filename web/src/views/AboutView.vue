<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import BrandMark from '../components/BrandMark.vue'
import Icon from '../components/Icon.vue'
import { errorMessage, get } from '../api'
import type { UpdateInfo } from '../types'
import { useAuthStore } from '../stores/auth'
import { useI18nStore } from '../stores/i18n'
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
const i18n = useI18nStore()
const t = i18n.t
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
    ui.toastError(err, t('about.notify.loadFailed'))
  } finally {
    loading.value = false
  }
}

async function checkUpdates() {
  checking.value = true
  try {
    update.value = await get<UpdateInfo>('/api/update/check?force=1')
    if (!update.value.ok) ui.notify('warning', t('about.notify.updateCheckFailed'), update.value.error)
    else if (update.value.updateAvailable) ui.notify('info', t('about.notify.newerRelease'), update.value.latestVersion)
    else ui.notify('success', t('about.notify.latestRelease'))
  } catch (err) {
    ui.toastError(err, t('about.notify.updateCheckFailed'))
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
        <span class="eyebrow">{{ t('about.eyebrow') }}</span>
        <h2>{{ t('about.title') }}</h2>
        <p>{{ t('about.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <a class="button" href="https://github.com/Antruly/uvscope" target="_blank" rel="noreferrer">
          <Icon name="star" :size="16" />{{ t('about.starOnGithub') }}
        </a>
      </div>
    </header>

    <section class="about-hero">
      <div class="about-hero__mark"><BrandMark :size="72" /></div>
      <div class="about-hero__copy">
        <span class="eyebrow">{{ t('about.versionLine', { version: about?.version || auth.version }) }}</span>
        <h1>{{ t('about.hero') }}</h1>
        <p>{{ about?.description || t('about.descriptionFallback') }}</p>
        <div class="chip-row">
          <span class="chip chip--accent"><Icon name="spark" :size="12" />{{ t('about.mitLicensed') }}</span>
          <span class="chip mono">libuvcpp {{ about?.libuvcpp || auth.libuvcpp }}</span>
          <span class="chip mono">libuv {{ about?.libuv }}</span>
          <span class="chip">{{ about?.platform }}</span>
        </div>
      </div>
      <div class="about-stars">
        <Icon name="star" :size="19" />
        <strong>{{ stars }}</strong>
        <span>{{ t('about.githubStars') }}</span>
      </div>
    </section>

    <div v-if="error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ error }}</div>

    <section class="about-grid">
      <article class="card about-update">
        <header class="card__head">
          <div><span class="eyebrow">{{ t('about.releaseChannel') }}</span><h3>{{ t('about.versionUpdates') }}</h3></div>
          <button class="button button--sm" :disabled="checking" @click="checkUpdates">
            <span v-if="checking" class="spinner" /><Icon v-else name="refresh" :size="14" />{{ t('about.check') }}
          </button>
        </header>
        <div class="version-line">
          <div class="version-badge"><span>{{ t('about.current') }}</span><strong>v{{ about?.version || auth.version }}</strong></div>
          <div class="version-arrow"><Icon name="arrow-right" :size="17" /></div>
          <div class="version-badge version-badge--next" :class="{ 'version-badge--available': update?.updateAvailable }">
            <span>{{ t('about.latest') }}</span><strong>{{ update?.latestVersion || t('about.notChecked') }}</strong>
          </div>
        </div>
        <div v-if="update" class="update-body">
          <div class="update-head">
            <span class="status-pill" :class="update.updateAvailable ? 'status-pill--warn' : 'status-pill--ok'">
              {{ update.updateAvailable ? t('about.updateAvailable') : t('about.upToDate') }}
            </span>
            <span class="muted mono">{{ update.checkedAt }}</span>
          </div>
          <p v-if="update.error" class="inline-alert inline-alert--danger">{{ update.error }}</p>
          <h4 v-if="update.releaseName">{{ update.releaseName }}</h4>
          <pre v-if="update.releaseNotes" class="release-notes">{{ update.releaseNotes }}</pre>
          <a v-if="update.htmlUrl" :href="update.htmlUrl" target="_blank" rel="noreferrer" class="button">
            <Icon name="external" :size="15" />{{ t('about.openReleaseNotes') }}
          </a>
        </div>
        <div v-else class="about-update__empty">
          <Icon name="download" :size="22" />
          <p>{{ t('about.noUpdates') }}</p>
        </div>
      </article>

      <article class="card">
        <header class="card__head"><div><span class="eyebrow">{{ t('about.stack') }}</span><h3>{{ t('about.whatsInside') }}</h3></div><Icon name="layers" :size="18" class="muted" /></header>
        <div class="stack-list">
          <div class="stack-row"><span class="stack-row__badge">C++</span><div><strong>libuvcpp 1.8.0</strong><p>{{ t('about.stack.libuvcpp') }}</p></div></div>
          <div class="stack-row"><span class="stack-row__badge">Vue</span><div><strong>Vue 3 + Vite</strong><p>{{ t('about.stack.vue') }}</p></div></div>
          <div class="stack-row"><span class="stack-row__badge">SQL</span><div><strong>SQLite WAL</strong><p>{{ t('about.stack.sqlite') }}</p></div></div>
          <div class="stack-row"><span class="stack-row__badge">TLS</span><div><strong>OpenSSL</strong><p>{{ t('about.stack.openssl') }}</p></div></div>
        </div>
      </article>

      <article class="card about-capabilities">
        <header class="card__head"><div><span class="eyebrow">{{ t('about.surface') }}</span><h3>{{ t('about.capabilities') }}</h3></div></header>
        <div class="capability-list">
          <span v-for="capability in about?.capabilities || []" :key="capability" class="capability-chip"><Icon name="check" :size="13" />{{ capability }}</span>
        </div>
        <div class="about-links">
          <a :href="about?.repository || 'https://github.com/Antruly/uvscope'" target="_blank" rel="noreferrer"><Icon name="globe" :size="15" />{{ t('about.repository') }}</a>
          <a :href="about?.issues || 'https://github.com/Antruly/uvscope/issues'" target="_blank" rel="noreferrer"><Icon name="alert" :size="15" />{{ t('about.issues') }}</a>
          <a href="https://github.com/Antruly/libuvcpp" target="_blank" rel="noreferrer"><Icon name="layers" :size="15" />libuvcpp</a>
        </div>
      </article>

      <article class="card about-cta">
        <div class="about-cta__glow" />
        <Icon name="star" :size="28" />
        <h3>{{ t('about.ctaTitle') }}</h3>
        <p>{{ t('about.ctaText') }}</p>
        <a class="button button--primary" href="https://github.com/Antruly/uvscope" target="_blank" rel="noreferrer">
          <Icon name="star" :size="16" />{{ t('about.ctaButton') }}
        </a>
      </article>
    </section>
  </div>
</template>
