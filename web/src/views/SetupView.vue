<script setup lang="ts">
import { computed, ref } from 'vue'
import BrandMark from '../components/BrandMark.vue'
import Icon from '../components/Icon.vue'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'
import { errorMessage } from '../api'

defineOptions({ name: 'SetupView' })
const emit = defineEmits<{ authenticated: [] }>()
const auth = useAuthStore()
const ui = useUiStore()

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
const strengthLabel = computed(() => ['Too short', 'Fair', 'Good', 'Strong', 'Excellent'][strength.value] || 'Too short')

async function submit() {
  error.value = ''
  if (username.value.trim().length < 3) {
    error.value = 'Username must be at least 3 characters.'
    return
  }
  if (password.value.length < 8) {
    error.value = 'Password must be at least 8 characters.'
    return
  }
  if (password.value !== confirm.value) {
    error.value = 'Passwords do not match.'
    return
  }
  submitting.value = true
  try {
    await auth.setup(username.value.trim(), password.value)
    ui.notify('success', 'Workbench initialized', 'Your administrator account is ready.')
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
        <span class="chip chip--warn">first run</span>
      </div>
      <div class="auth-art__copy">
        <p class="eyebrow">Fresh installation</p>
        <h1>
          Your workbench.<br />
          <span class="text-gradient">Your rules.</span>
        </h1>
        <p class="auth-art__lead">
          uvscope stores everything locally in SQLite. Create the first administrator, then decide which
          protocol modules and gateway controls are available to your team.
        </p>
      </div>
      <div class="setup-steps">
        <div class="setup-step setup-step--active">
          <span>01</span>
          <div><strong>Create administrator</strong><p>Owns users, roles, settings and reset.</p></div>
        </div>
        <div class="setup-step">
          <span>02</span>
          <div><strong>Choose modules</strong><p>Toggle protocol surfaces after sign-in.</p></div>
        </div>
        <div class="setup-step">
          <span>03</span>
          <div><strong>Start probing</strong><p>Or launch your first reverse proxy.</p></div>
        </div>
      </div>
      <div class="auth-art__foot">
        <span><i class="status-dot" /> no telemetry, no cloud</span>
        <span class="mono">{{ auth.dataDir }}</span>
      </div>
    </section>

    <section class="auth-panel">
      <button class="icon-button auth-theme" :title="ui.theme === 'dark' ? 'Light theme' : 'Dark theme'" @click="ui.toggleTheme">
        <Icon :name="ui.theme === 'dark' ? 'sun' : 'moon'" :size="18" />
      </button>
      <form class="auth-card" @submit.prevent="submit">
        <div class="auth-card__head">
          <span class="eyebrow">Initialize</span>
          <h2>Create the administrator</h2>
          <p>This account is created once. Recovery is available from Settings.</p>
        </div>

        <label class="field">
          <span>Username</span>
          <div class="input-shell">
            <Icon name="users" :size="17" />
            <input v-model="username" autocomplete="username" placeholder="admin" autofocus />
          </div>
        </label>

        <label class="field">
          <span>Password</span>
          <div class="input-shell">
            <Icon name="lock" :size="17" />
            <input v-model="password" type="password" autocomplete="new-password" placeholder="At least 8 characters" />
          </div>
        </label>

        <div class="strength">
          <div class="strength__bars">
            <span v-for="n in 4" :key="n" :class="{ filled: strength >= n }" />
          </div>
          <small>{{ strengthLabel }}</small>
        </div>

        <label class="field">
          <span>Confirm password</span>
          <div class="input-shell">
            <Icon name="check" :size="17" />
            <input v-model="confirm" type="password" autocomplete="new-password" placeholder="Repeat the password" />
          </div>
        </label>

        <div v-if="error" class="inline-alert inline-alert--danger">
          <Icon name="alert" :size="16" />
          <span>{{ error }}</span>
        </div>

        <button class="button button--primary button--lg auth-submit" :disabled="submitting">
          <span v-if="submitting" class="spinner" />
          <Icon v-else name="spark" :size="17" />
          {{ submitting ? 'Initializing…' : 'Initialize workbench' }}
        </button>
        <div class="auth-card__meta">
          <span>Storage</span>
          <span class="mono">SQLite · {{ auth.dataDir }}</span>
        </div>
      </form>
    </section>
  </div>
</template>
