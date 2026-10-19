import { ref } from 'vue'
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

export const useUiStore = defineStore('ui', () => {
  const theme = ref<'dark' | 'light'>(storedTheme === 'light' ? 'light' : 'dark')
  const toasts = ref<Toast[]>([])
  const sidebarCollapsed = ref(false)
  const mobileNavOpen = ref(false)
  let toastId = 1

  function applyTheme() {
    document.documentElement.dataset.theme = theme.value
    document.documentElement.style.colorScheme = theme.value
    try {
      localStorage.setItem('uvscope-theme', theme.value)
    } catch {
      // Theme is a convenience; private-mode storage failures must not break the UI.
    }
  }

  function toggleTheme() {
    theme.value = theme.value === 'dark' ? 'light' : 'dark'
    applyTheme()
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
    toasts,
    sidebarCollapsed,
    mobileNavOpen,
    toggleTheme,
    notify,
    dismiss,
    toastError,
    applyTheme,
  }
})
