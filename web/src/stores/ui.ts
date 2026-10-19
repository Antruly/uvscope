import { computed, ref } from 'vue'
import { defineStore } from 'pinia'

export type ToastKind = 'success' | 'error' | 'info' | 'warning'
export interface Toast {
  id: number
  kind: ToastKind
  title: string
  message?: string
}

const storedTheme = (() => {
  try {
    return localStorage.getItem('uvscope-theme')
  } catch {
    return null
  }
})()

export type ThemeMode = 'dark' | 'light' | 'auto'

const validTheme = (value: string | null): ThemeMode =>
  value === 'light' || value === 'auto' ? value : 'dark'

const systemDark = ref(
  typeof window !== 'undefined' && window.matchMedia
    ? window.matchMedia('(prefers-color-scheme: dark)').matches
    : true,
)

export const useUiStore = defineStore('ui', () => {
  const theme = ref<ThemeMode>(validTheme(storedTheme))
  const toasts = ref<Toast[]>([])
  const sidebarCollapsed = ref(false)
  const mobileNavOpen = ref(false)
  let toastId = 1

  const resolvedTheme = computed<'dark' | 'light'>(() =>
    theme.value === 'auto' ? (systemDark.value ? 'dark' : 'light') : theme.value,
  )

  const themeTitle = computed(() => {
    if (theme.value === 'auto') return `Theme: auto (${resolvedTheme.value}) — click for dark`
    return theme.value === 'dark' ? 'Theme: dark — click for light' : 'Theme: light — click for auto'
  })

  const themeIcon = computed(() =>
    theme.value === 'auto' ? 'monitor' : resolvedTheme.value === 'dark' ? 'sun' : 'moon',
  )

  function applyTheme() {
    document.documentElement.dataset.theme = resolvedTheme.value
    document.documentElement.style.colorScheme = resolvedTheme.value
    try {
      localStorage.setItem('uvscope-theme', theme.value)
    } catch {
      // Theme is a convenience; private-mode storage failures must not break the UI.
    }
  }

  function cycleTheme() {
    theme.value = theme.value === 'dark' ? 'light' : theme.value === 'light' ? 'auto' : 'dark'
    applyTheme()
  }

  function toggleTheme() {
    theme.value = resolvedTheme.value === 'dark' ? 'light' : 'dark'
    applyTheme()
  }

  if (typeof window !== 'undefined' && window.matchMedia) {
    const query = window.matchMedia('(prefers-color-scheme: dark)')
    const onChange = (event: MediaQueryListEvent) => {
      systemDark.value = event.matches
      if (theme.value === 'auto') applyTheme()
    }
    if (query.addEventListener) query.addEventListener('change', onChange)
    else query.addListener(onChange)
  }

  function notify(kind: ToastKind, title: string, message?: string) {
    const id = toastId++
    toasts.value.push({ id, kind, title, message })
    window.setTimeout(() => dismiss(id), 4200)
  }

  function dismiss(id: number) {
    toasts.value = toasts.value.filter((toast) => toast.id !== id)
  }

  function toastError(error: unknown, title = 'Something went wrong') {
    notify('error', title, error instanceof Error ? error.message : String(error))
  }

  applyTheme()

  return {
    theme,
    resolvedTheme,
    themeTitle,
    themeIcon,
    toasts,
    sidebarCollapsed,
    mobileNavOpen,
    cycleTheme,
    toggleTheme,
    notify,
    dismiss,
    toastError,
    applyTheme,
  }
})
