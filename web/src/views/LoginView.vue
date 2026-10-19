<script setup lang="ts">
import { computed, ref } from 'vue'
import BrandMark from '../components/BrandMark.vue'
import Icon from '../components/Icon.vue'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'
import { errorMessage } from '../api'

defineOptions({ name: 'LoginView' })
const emit = defineEmits<{ authenticated: [] }>()
const auth = useAuthStore()
const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
const themeTitle = computed(() => {
  const mode = ui.theme === 'auto'
    ? `${t('theme.auto')} (${t(`theme.${ui.resolvedTheme}`)})`
    : t(`theme.${ui.theme}`)
  return t('theme.switch', { mode })
})

const username = ref('')
const password = ref('')
const error = ref('')
const submitting = ref(false)

async function submit() {
  error.value = ''
  if (!username.value || !password.value) {
    error.value = t('auth.login.required')
    return
  }
  submitting.value = true
  try {
    await auth.login(username.value.trim(), password.value)
    ui.notify('success', t('auth.login.welcome'), `${auth.user?.username} · ${auth.user?.role}`)
    emit('authenticated')
  } catch (err) {
    error.value = errorMessage(err)
  } finally {
    submitting.value = false
  }
}
</script>

<template>
  <div class="auth-page">
    <div class="auth-page__noise" />
    <section class="auth-art">
      <div class="auth-art__top">
        <BrandMark :size="40" />
        <span class="auth-wordmark">uvscope</span>
        <span class="chip chip--accent">v{{ auth.version }} {{ t('auth.preview') }}</span>
      </div>
      <div class="auth-art__copy">
        <p class="eyebrow">{{ t('auth.login.eyebrow') }}</p>
        <h1>
          {{ t('auth.login.title1') }}<br />
          <span class="text-gradient">{{ t('auth.login.title2') }}</span>
        </h1>
        <p class="auth-art__lead">{{ t('auth.login.lead') }}</p>
      </div>
      <div class="auth-art__grid">
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="bolt" :size="18" /></span>
          <div><strong>{{ t('auth.login.feature1.title') }}</strong><p>{{ t('auth.login.feature1.text') }}</p></div>
        </div>
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="route" :size="18" /></span>
          <div><strong>{{ t('auth.login.feature2.title') }}</strong><p>{{ t('auth.login.feature2.text') }}</p></div>
        </div>
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="database" :size="18" /></span>
          <div><strong>{{ t('auth.login.feature3.title') }}</strong><p>{{ t('auth.login.feature3.text') }}</p></div>
        </div>
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="shield" :size="18" /></span>
          <div><strong>{{ t('auth.login.feature4.title') }}</strong><p>{{ t('auth.login.feature4.text') }}</p></div>
        </div>
      </div>
      <div class="auth-art__foot">
        <span><i class="status-dot" /> {{ t('auth.login.localReady') }}</span>
        <span class="mono">libuvcpp {{ auth.libuvcpp || '1.8.0' }}</span>
      </div>
    </section>

    <section class="auth-panel">
      <div class="auth-actions">
        <button class="icon-button locale-toggle" :title="t('language.switch')" @click="i18n.toggleLocale">{{ i18n.nextLabel }}</button>
        <button class="icon-button auth-theme" :title="themeTitle" @click="ui.cycleTheme">
          <Icon :name="ui.themeIcon" :size="18" />
        </button>
      </div>
      <form class="auth-card" @submit.prevent="submit">
        <div class="auth-card__head">
          <span class="eyebrow">{{ t('auth.login.eyebrowCard') }}</span>
          <h2>{{ t('auth.login.title') }}</h2>
          <p>{{ t('auth.login.leadCard') }}</p>
        </div>

        <label class="field">
          <span>{{ t('auth.login.username') }}</span>
          <div class="input-shell">
            <Icon name="users" :size="17" />
            <input v-model="username" autocomplete="username" placeholder="admin" autofocus />
          </div>
        </label>

        <label class="field">
          <span>{{ t('auth.login.password') }}</span>
          <div class="input-shell">
            <Icon name="lock" :size="17" />
            <input v-model="password" type="password" autocomplete="current-password" placeholder="••••••••" />
          </div>
        </label>

        <div v-if="error" class="inline-alert inline-alert--danger">
          <Icon name="alert" :size="16" />
          <span>{{ error }}</span>
        </div>

        <button class="button button--primary button--lg auth-submit" :disabled="submitting">
          <span v-if="submitting" class="spinner" />
          <Icon v-else name="arrow-right" :size="17" />
          {{ submitting ? t('auth.login.signingIn') : t('auth.login.enter') }}
        </button>

        <div class="auth-card__meta">
          <span>{{ t('auth.login.firstInstall') }}</span>
          <span class="mono">{{ auth.dataDir }}</span>
        </div>
      </form>
    </section>
  </div>
</template>
