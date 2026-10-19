import { computed, ref } from 'vue'
import { defineStore } from 'pinia'
import type { RouteLocationNormalizedLoaded } from 'vue-router'

export interface AppTab {
  name: string
  title: string
  titleKey?: string
  path: string
  icon: string
  componentName: string
  module?: string
  closable: boolean
}

export const useTabsStore = defineStore('tabs', () => {
  const tabs = ref<AppTab[]>([
    {
      name: 'dashboard',
      title: 'Overview',
      titleKey: 'nav.overview',
      path: '/',
      icon: 'dashboard',
      componentName: 'DashboardView',
      closable: false,
    },
  ])
  const active = ref('dashboard')

  const cachedComponents = computed(() =>
    tabs.value.map((tab) => tab.componentName).filter((name) => Boolean(name)),
  )

  function openFromRoute(route: RouteLocationNormalizedLoaded) {
    const name = typeof route.name === 'string' ? route.name : route.path
    if (!name || route.meta.public) return
    const existing = tabs.value.find((tab) => tab.name === name)
    if (existing) {
      existing.path = route.fullPath
      active.value = name
      return
    }
    tabs.value.push({
      name,
      title: (route.meta.title as string) || name,
      titleKey: route.meta.titleKey as string | undefined,
      path: route.fullPath,
      icon: (route.meta.icon as string) || 'dot',
      componentName: (route.meta.componentName as string) || '',
      module: route.meta.module as string | undefined,
      closable: name !== 'dashboard',
    })
    active.value = name
  }

  function close(name: string): string | null {
    const index = tabs.value.findIndex((tab) => tab.name === name)
    if (index < 0) return null
    const tab = tabs.value[index]
    if (!tab.closable) return null
    tabs.value.splice(index, 1)
    if (active.value !== name) return null
    const next = tabs.value[Math.max(0, index - 1)] ?? tabs.value[0]
    active.value = next?.name ?? 'dashboard'
    return next?.path ?? '/'
  }

  function closeOthers(name: string): string | null {
    tabs.value = tabs.value.filter((tab) => !tab.closable || tab.name === name)
    active.value = name
    return tabs.value.find((tab) => tab.name === name)?.path ?? '/'
  }

  function closeAll(): string {
    tabs.value = tabs.value.filter((tab) => !tab.closable)
    active.value = 'dashboard'
    return '/'
  }

  function setActive(name: string) {
    active.value = name
  }

  return { tabs, active, cachedComponents, openFromRoute, close, closeOthers, closeAll, setActive }
})
