<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import BrandMark from './BrandMark.vue'
import Icon from './Icon.vue'
import { useAuthStore } from '../stores/auth'
import { useTabsStore } from '../stores/tabs'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'AppShell' })

interface NavItem {
  label: string
  icon: string
  to: string
  module?: string
  admin?: 'users' | 'settings'
}

const auth = useAuthStore()
const tabs = useTabsStore()
const ui = useUiStore()
const route = useRoute()
const router = useRouter()

const userMenuOpen = ref(false)
const userMenuRef = ref<HTMLElement | null>(null)

const sections = computed(() => {
  const workspace: NavItem[] = [
    { label: 'Overview', icon: 'dashboard', to: '/' },
    { label: 'HTTP Studio', icon: 'bolt', to: '/http', module: 'http' },
    { label: 'WebSocket', icon: 'pulse', to: '/websocket', module: 'websocket' },
    { label: 'TCP / UDP', icon: 'layers', to: '/transport', module: 'tcpudp' },
    { label: 'SOAP / WSDL', icon: 'braces', to: '/soap', module: 'soap' },
    { label: 'TLS Inspector', icon: 'shield', to: '/tls', module: 'tls' },
  ].filter((item) => !item.module || auth.moduleEnabled(item.module as keyof typeof auth.modules))

  const gateway: NavItem[] = [{ label: 'Reverse Proxy', icon: 'route', to: '/proxy', module: 'proxy' }].filter(
    (item) => !item.module || auth.moduleEnabled(item.module as keyof typeof auth.modules),
  )

  const system: NavItem[] = [
    ...(auth.canAdminUsers ? [{ label: 'Users & Roles', icon: 'users', to: '/users' }] : []),
    ...(auth.canAdminSettings ? [{ label: 'Settings', icon: 'settings', to: '/settings' }] : []),
    { label: 'About', icon: 'spark', to: '/about' },
    { label: 'Help', icon: 'help', to: '/help' },
  ]

  return [
    { title: 'Workspace', items: workspace },
    { title: 'Gateway', items: gateway },
    { title: 'System', items: system },
  ].filter((section) => section.items.length > 0)
})

const routeTitle = computed(() => (route.meta.title as string) || 'uvscope')
const isActive = (to: string) => (to === '/' ? route.path === '/' : route.path.startsWith(to))

watch(
  () => route.fullPath,
  () => {
    const meta = route.meta
    if (meta.module && !auth.moduleEnabled(meta.module as keyof typeof auth.modules)) {
      ui.notify('warning', 'Module disabled', `${routeTitle.value} is disabled in Settings.`)
      router.replace('/')
      return
    }
    if (meta.admin === 'users' && !auth.canAdminUsers) {
      ui.notify('warning', 'Administrator required')
      router.replace('/')
      return
    }
    if (meta.admin === 'settings' && !auth.canAdminSettings) {
      ui.notify('warning', 'Administrator required')
      router.replace('/')
      return
    }
    tabs.openFromRoute(route)
    ui.mobileNavOpen = false
  },
  { immediate: true },
)

function go(to: string) {
  userMenuOpen.value = false
  ui.mobileNavOpen = false
  router.push(to)
}

function activateTab(name: string, path: string) {
  tabs.setActive(name)
  if (route.fullPath !== path) router.push(path)
}

function closeTab(name: string) {
  const next = tabs.close(name)
  if (next) router.push(next)
}

function closeOthers(name: string) {
  const next = tabs.closeOthers(name)
  if (next) router.push(next)
}

async function logout() {
  userMenuOpen.value = false
  await auth.logout()
  ui.notify('info', 'Signed out')
}

function onDocumentClick(event: MouseEvent) {
  if (userMenuOpen.value && userMenuRef.value && !userMenuRef.value.contains(event.target as Node)) {
    userMenuOpen.value = false
  }
}

onMounted(() => document.addEventListener('click', onDocumentClick))
onBeforeUnmount(() => document.removeEventListener('click', onDocumentClick))
</script>

<template>
  <div class="shell" :class="{ 'shell--collapsed': ui.sidebarCollapsed, 'shell--nav-open': ui.mobileNavOpen }">
    <div class="mobile-scrim" @click="ui.mobileNavOpen = false" />
    <aside class="sidebar">
      <div class="sidebar__brand">
        <button class="brand-button" aria-label="Go to overview" @click="go('/')">
          <BrandMark :size="34" />
          <span class="brand-copy">
            <strong>uvscope</strong>
            <small>protocol workbench</small>
          </span>
        </button>
        <button
          class="icon-button sidebar__collapse"
          :title="ui.sidebarCollapsed ? 'Expand sidebar' : 'Collapse sidebar'"
          @click="ui.sidebarCollapsed = !ui.sidebarCollapsed"
        >
          <Icon :name="ui.sidebarCollapsed ? 'chevron-right' : 'chevron-left'" :size="16" />
        </button>
      </div>

      <nav class="sidebar__nav" aria-label="Primary">
        <section v-for="section in sections" :key="section.title" class="nav-section">
          <div class="nav-section__label">{{ section.title }}</div>
          <button
            v-for="item in section.items"
            :key="item.to"
            class="nav-item"
            :class="{ 'nav-item--active': isActive(item.to) }"
            :title="ui.sidebarCollapsed ? item.label : undefined"
            @click="go(item.to)"
          >
            <span class="nav-item__icon"><Icon :name="item.icon" :size="18" /></span>
            <span class="nav-item__label">{{ item.label }}</span>
          </button>
        </section>
      </nav>

      <div class="sidebar__foot">
        <div class="daemon-chip">
          <span class="status-dot" />
          <span class="daemon-chip__copy">
            <strong>daemon online</strong>
            <small>v{{ auth.version }} · libuvcpp {{ auth.libuvcpp || '1.8.0' }}</small>
          </span>
        </div>
      </div>
    </aside>

    <main class="main">
      <header class="topbar">
        <button class="icon-button mobile-menu" aria-label="Open navigation" @click="ui.mobileNavOpen = true">
          <Icon name="menu" :size="19" />
        </button>
        <div class="topbar__title">
          <span class="topbar__eyebrow">uvscope / workspace</span>
          <h1>{{ routeTitle }}</h1>
        </div>
        <div class="topbar__spacer" />
        <a class="ph-badge" href="https://github.com/Antruly/uvscope" target="_blank" rel="noreferrer">
          <Icon name="spark" :size="14" />
          <span>0.1.0 preview</span>
        </a>
        <button class="icon-button" :title="ui.theme === 'dark' ? 'Switch to light theme' : 'Switch to dark theme'" @click="ui.toggleTheme">
          <Icon :name="ui.theme === 'dark' ? 'sun' : 'moon'" :size="18" />
        </button>
        <div ref="userMenuRef" class="user-menu">
          <button class="user-button" :aria-expanded="userMenuOpen" @click.stop="userMenuOpen = !userMenuOpen">
            <span class="avatar">{{ (auth.user?.username || 'u').slice(0, 1).toUpperCase() }}</span>
            <span class="user-button__copy">
              <strong>{{ auth.user?.username }}</strong>
              <small>{{ auth.user?.role }}</small>
            </span>
            <Icon name="chevron-down" :size="14" />
          </button>
          <Transition name="menu">
            <div v-if="userMenuOpen" class="dropdown">
              <div class="dropdown__head">
                <strong>{{ auth.user?.username }}</strong>
                <span>{{ auth.user?.roleDescription }}</span>
              </div>
              <button class="dropdown__item" @click="go('/about')"><Icon name="spark" :size="16" />About uvscope</button>
              <button v-if="auth.canAdminSettings" class="dropdown__item" @click="go('/settings')">
                <Icon name="settings" :size="16" />Settings
              </button>
              <div class="dropdown__sep" />
              <button class="dropdown__item dropdown__item--danger" @click="logout">
                <Icon name="logout" :size="16" />Sign out
              </button>
            </div>
          </Transition>
        </div>
      </header>

      <div class="tabbar" role="tablist" aria-label="Open workspaces">
        <div class="tabbar__scroll">
          <button
            v-for="tab in tabs.tabs"
            :key="tab.name"
            class="tab"
            :class="{ 'tab--active': tabs.active === tab.name }"
            role="tab"
            :aria-selected="tabs.active === tab.name"
            @click="activateTab(tab.name, tab.path)"
            @contextmenu.prevent="tab.closable && closeOthers(tab.name)"
          >
            <Icon :name="tab.icon" :size="15" />
            <span>{{ tab.title }}</span>
            <span
              v-if="tab.closable"
              class="tab__close"
              role="button"
              tabindex="0"
              aria-label="Close tab"
              @click.stop="closeTab(tab.name)"
              @keydown.enter.stop="closeTab(tab.name)"
            >
              <Icon name="close" :size="12" />
            </span>
          </button>
        </div>
        <div class="tabbar__actions">
          <span class="tabbar__hint">right-click a tab to close the rest</span>
        </div>
      </div>

      <div class="content">
        <RouterView v-slot="{ Component }">
          <KeepAlive :include="tabs.cachedComponents">
            <component :is="Component" />
          </KeepAlive>
        </RouterView>
      </div>
    </main>
  </div>
</template>

