import { computed, ref } from 'vue'
import { defineStore } from 'pinia'
import { detectLocale, dictionaries, type Locale } from '../i18n'

export type { Locale }

export const useI18nStore = defineStore('i18n', () => {
  const locale = ref<Locale>(detectLocale())

  function translate(key: string, params?: Record<string, string | number>): string {
    const dictionary = dictionaries[locale.value] || dictionaries.en
    let text = dictionary[key] ?? dictionaries.en[key] ?? key
    if (params) {
      for (const [name, value] of Object.entries(params)) {
        text = text.split(`{${name}}`).join(String(value))
      }
    }
    return text
  }

  function applyLocale() {
    if (typeof document !== 'undefined') document.documentElement.lang = locale.value
    try {
      localStorage.setItem('uvscope-locale', locale.value)
    } catch {
      // Language is a convenience; private-mode storage failures must not break the UI.
    }
  }

  function setLocale(next: Locale) {
    locale.value = next
    applyLocale()
  }

  function toggleLocale() {
    setLocale(locale.value === 'zh-CN' ? 'en' : 'zh-CN')
  }

  const label = computed(() => (locale.value === 'zh-CN' ? '中文' : 'EN'))
  const nextLabel = computed(() => (locale.value === 'zh-CN' ? 'EN' : '中文'))

  applyLocale()

  return { locale, t: translate, setLocale, toggleLocale, label, nextLabel }
})
