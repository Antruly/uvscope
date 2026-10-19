<script setup lang="ts">
import { computed, ref } from 'vue'
import BrandMark from '../components/BrandMark.vue'
import Icon from '../components/Icon.vue'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'
import { errorMessage } from '../api'

defineOptions({ name: 'SetupView' })
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
const confirm = ref('')
const error = ref('')
const submitting = ref(false)

const strength = computed(() => {
  let score = 0
  if (password.value.length >= 8) score++
  if (password.value.length >= 12) score++
  if (/[A-Z]/.test(password.value) && /[a-z]/.test(password.value)) score++
  if (/\d/.test(password.value) || /[^A-Za-z0-9]/.test(password.value)) score++
  return score
})
const strengthLabel = computed(() => t(`auth.setup.strength.${strength.value}`))

async function submit() {
  error.value = ''
  if (username.value.trim().length < 3) {
    error.value = t('auth.setup.usernameShort')
    return
  }
  if (password.value.length < 8) {
    error.value = t('auth.setup.passwordShort')
    return
  }
  if (password.value !== confirm.value) {
    error.value = t('auth.setup.mismatch')
    return
  }
  submitting.value = true
  try {
    await auth.setup(username.value.trim(), password.value)
    ui.notify('success', t('auth.setup.success'), t('auth.setup.successBody'))
    emit('authenticated')
  } catch (err) {
    error.value = errorMessage(err)
  } finally {
    submitting.value = false
  }
}
</script>

<template>
  <div class="auth-page auth-page--setup">
    <div class="auth-page__noise" />
    <section class="auth-art">
      <div class="auth-art__top">
        <BrandMark :size="40" />
        <span class="auth-wordmark">uvscope</span>
        <span class="chip chip--warn">{{ t('auth.setup.chip') }}</span>
      </div>
      <div class="auth-art__copy">
        <p class="eyebrow">{{ t('auth.setup.eyebrow') }}</p>
        <h1>
          {{ t('auth.setup.title1') }}<br />
          <span class="text-gradient">{{ t('auth.setup.title2') }}</span>
        </h1>
        <p class="auth-art__lead">{{ t('auth.setup.lead') }}</p>
      </div>
      <div class="setup-steps">
        <div class="setup-step setup-step--active">
          <span>01</span>
          <div><strong>{{ t('auth.setup.step1.title') }}</strong><p>{{ t('auth.setup.step1.text') }}</p></div>
        </div>
        <div class="setup-step">
          <span>02</span>
          <div><strong>{{ t('auth.setup.step2.title') }}</strong><p>{{ t('auth.setup.step2.text') }}</p></div>
        </div>
        <div class="setup-step">
          <span>03</span>
          <div><strong>{{ t('auth.setup.step3.title') }}</strong><p>{{ t('auth.setup.step3.text') }}</p></div>
        </div>
      </div>
      <div class="auth-art__foot">
        <span><i class="status-dot" /> {{ t('auth.setup.noTelemetry') }}</span>
        <span class="mono">{{ auth.dataDir }}</span>
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
          <span class="eyebrow">{{ t('auth.setup.eyebrowCard') }}</span>
          <h2>{{ t('auth.setup.title') }}</h2>
          <p>{{ t('auth.setup.leadCard') }}</p>
        </div>

        <label class="field">
          <span>{{ t('auth.setup.username') }}</span>
          <div class="input-shell">
            <Icon name="users" :size="17" />
            <input v-model="username" autocomplete="username" placeholder="admin" autofocus />
          </div>
        </label>

        <label class="field">
          <span>{{ t('auth.setup.password') }}</span>
          <div class="input-shell">
            <Icon name="lock" :size="17" />
            <input v-model="password" type="password" autocomplete="new-password" :placeholder="t('auth.setup.passwordHint')" />
          </div>
        </label>

        <div class="strength">
          <div class="strength__bars">
            <span v-for="n in 4" :key="n" :class="{ filled: strength >= n }" />
          </div>
          <small>{{ strengthLabel }}</small>
        </div>

        <label class="field">
          <span>{{ t('auth.setup.confirm') }}</span>
          <div class="input-shell">
            <Icon name="check" :size="17" />
            <input v-model="confirm" type="password" autocomplete="new-password" :placeholder="t('auth.setup.confirmHint')" />
          </div>
        </label>

        <div v-if="error" class="inline-alert inline-alert--danger">
          <Icon name="alert" :size="16" />
          <span>{{ error }}</span>
        </div>

        <button class="button button--primary button--lg auth-submit" :disabled="submitting">
          <span v-if="submitting" class="spinner" />
          <Icon v-else name="spark" :size="17" />
          {{ submitting ? t('auth.setup.initializing') : t('auth.setup.initialize') }}
        </button>
        <div class="auth-card__meta">
          <span>{{ t('auth.setup.storage') }}</span>
          <span class="mono">SQLite · {{ auth.dataDir }}</span>
        </div>
      </form>
    </section>
  </div>
</template>
