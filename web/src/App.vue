<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import AppShell from './components/AppShell.vue'
import ToastHost from './components/ToastHost.vue'
import LoginView from './views/LoginView.vue'
import SetupView from './views/SetupView.vue'
import BrandMark from './components/BrandMark.vue'
import { useAuthStore } from './stores/auth'
import { useTabsStore } from './stores/tabs'
import { useUiStore } from './stores/ui'
import { useI18nStore } from './stores/i18n'

const auth = useAuthStore()
const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
const tabs = useTabsStore()
const router = useRouter()
const loading = ref(true)

async function bootstrap() {
  loading.value = true
  await router.isReady()
  const state = await auth.initialize()
  loading.value = false
  if (state === 'ready') {
    await router.replace(router.currentRoute.value.path === '/' ? '/' : router.currentRoute.value.fullPath)
  } else if (state === 'login' && auth.statusError) {
    ui.notify('error', t('app.cannotReach'), auth.statusError)
  }
}

function authenticated() {
  if (router.currentRoute.value.path === '/') {
    tabs.openFromRoute(router.currentRoute.value)
  }
  router.replace('/')
}

function unauthorized() {
  if (!auth.isAuthenticated) return
  auth.clearSession()
  ui.notify('warning', t('shell.sessionExpired'), t('shell.sessionExpiredBody'))
  router.replace('/')
}

onMounted(() => {
  window.addEventListener('uvscope:unauthorized', unauthorized)
  void bootstrap()
})

onBeforeUnmount(() => window.removeEventListener('uvscope:unauthorized', unauthorized))
</script>

<template>
  <div class="app-root">
    <div v-if="loading" class="splash">
      <div class="splash__glow" />
      <BrandMark :size="58" />
      <div class="splash__word">uvscope</div>
      <div class="splash__line"><span /></div>
      <p>{{ t('app.warming') }}</p>
    </div>

    <SetupView v-else-if="auth.setupRequired" @authenticated="authenticated" />
    <LoginView v-else-if="!auth.isAuthenticated" @authenticated="authenticated" />
    <AppShell v-else />
    <ToastHost />
  </div>
</template>
