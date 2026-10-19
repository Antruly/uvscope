<script setup lang="ts">
import { ref } from 'vue'
import BrandMark from '../components/BrandMark.vue'
import Icon from '../components/Icon.vue'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'
import { errorMessage } from '../api'

defineOptions({ name: 'LoginView' })
const emit = defineEmits<{ authenticated: [] }>()
const auth = useAuthStore()
const ui = useUiStore()

const username = ref('')
const password = ref('')
const error = ref('')
const submitting = ref(false)

async function submit() {
  error.value = ''
  if (!username.value || !password.value) {
    error.value = 'Enter your username and password.'
    return
  }
  submitting.value = true
  try {
    await auth.login(username.value.trim(), password.value)
    ui.notify('success', 'Welcome back', `${auth.user?.username} · ${auth.user?.role}`)
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
        <span class="chip chip--accent">v{{ auth.version }} preview</span>
      </div>
      <div class="auth-art__copy">
        <p class="eyebrow">The multi-protocol workbench</p>
        <h1>
          One scope for every<br />
          <span class="text-gradient">web handshake.</span>
        </h1>
        <p class="auth-art__lead">
          Inspect HTTP, WebSocket, TCP, UDP, TLS and SOAP from a single local control plane, then put
          traffic to work with programmable reverse proxies.
        </p>
      </div>
      <div class="auth-art__grid">
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="bolt" :size="18" /></span>
          <div><strong>Protocol-native</strong><p>Raw frames, headers, timing and bytes.</p></div>
        </div>
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="route" :size="18" /></span>
          <div><strong>Gateway built in</strong><p>Managed HTTP reverse proxies with live logs.</p></div>
        </div>
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="database" :size="18" /></span>
          <div><strong>Local by design</strong><p>SQLite storage, RBAC and no cloud dependency.</p></div>
        </div>
        <div class="auth-feature">
          <span class="auth-feature__icon"><Icon name="shield" :size="18" /></span>
          <div><strong>libuvcpp engine</strong><p>One event-driven C++ stack end to end.</p></div>
        </div>
      </div>
      <div class="auth-art__foot">
        <span><i class="status-dot" /> local daemon ready</span>
        <span class="mono">libuvcpp {{ auth.libuvcpp || '1.8.0' }}</span>
      </div>
    </section>

    <section class="auth-panel">
      <button class="icon-button auth-theme" :title="ui.theme === 'dark' ? 'Light theme' : 'Dark theme'" @click="ui.toggleTheme">
        <Icon :name="ui.theme === 'dark' ? 'sun' : 'moon'" :size="18" />
      </button>
      <form class="auth-card" @submit.prevent="submit">
        <div class="auth-card__head">
          <span class="eyebrow">Control plane</span>
          <h2>Sign in to uvscope</h2>
          <p>Use the account provisioned on this machine.</p>
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
          {{ submitting ? 'Signing in…' : 'Enter workspace' }}
        </button>

        <div class="auth-card__meta">
          <span>First install?</span>
          <span class="mono">{{ auth.dataDir }}</span>
        </div>
      </form>
    </section>
  </div>
</template>
