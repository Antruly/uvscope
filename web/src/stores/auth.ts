import { computed, ref } from 'vue'
import { defineStore } from 'pinia'
import { api, errorMessage, get, post } from '../api'
import type { AuthResponse, ModuleMap, SetupStatus, User } from '../types'

const allModules: ModuleMap = {
  http: true,
  websocket: true,
  tcpudp: true,
  soap: true,
  tls: true,
  proxy: true,
  terminal: false,
}

export const useAuthStore = defineStore('auth', () => {
  const user = ref<User | null>(null)
  const modules = ref<ModuleMap>({ ...allModules })
  const permissions = ref<Record<string, boolean>>({})
  const ready = ref(false)
  const setupRequired = ref(false)
  const initialized = ref(false)
  const loading = ref(false)
  const version = ref('0.3.0')
  const libuvcpp = ref('')
  const dataDir = ref('')
  const statusError = ref('')

  const isAuthenticated = computed(() => user.value !== null)
  const isAdmin = computed(() => user.value?.role === 'admin')
  const canAdminUsers = computed(() => permissions.value['admin.users'] ?? isAdmin.value)
  const canAdminSettings = computed(() => permissions.value['admin.settings'] ?? isAdmin.value)

  function applyAuth(response: AuthResponse) {
    user.value = response.user
    if (response.modules) modules.value = { ...allModules, ...response.modules }
    if (response.permissions) permissions.value = response.permissions
  }

  async function initialize(): Promise<'ready' | 'setup' | 'login'> {
    ready.value = false
    statusError.value = ''
    try {
      const status = await get<SetupStatus>('/api/setup/status')
      version.value = status.version
      libuvcpp.value = status.libuvcpp
      dataDir.value = status.dataDir
      initialized.value = status.initialized
      setupRequired.value = status.setupRequired
      if (status.setupRequired) {
        user.value = null
        ready.value = true
        return 'setup'
      }
      try {
        const me = await get<AuthResponse>('/api/auth/me')
        applyAuth(me)
        ready.value = true
        return 'ready'
      } catch {
        user.value = null
        ready.value = true
        return 'login'
      }
    } catch (error) {
      statusError.value = errorMessage(error)
      ready.value = true
      return 'login'
    }
  }

  async function login(username: string, password: string) {
    loading.value = true
    try {
      const response = await post<AuthResponse>('/api/auth/login', { username, password })
      applyAuth(response)
      return response
    } finally {
      loading.value = false
    }
  }

  async function setup(username: string, password: string) {
    loading.value = true
    try {
      const response = await post<AuthResponse>('/api/setup/init', { username, password })
      setupRequired.value = false
      initialized.value = true
      applyAuth(response)
      return response
    } finally {
      loading.value = false
    }
  }

  async function logout() {
    try {
      await post('/api/auth/logout')
    } catch {
      // The local session is authoritative for the UI; clearing it is enough.
    }
    user.value = null
    permissions.value = {}
  }

  async function refresh() {
    if (!user.value) return
    const me = await api<AuthResponse>('/api/auth/me')
    applyAuth(me)
  }

  function clearSession() {
    user.value = null
    permissions.value = {}
  }

  function moduleEnabled(module: keyof ModuleMap): boolean {
    return modules.value[module] !== false
  }

  return {
    user,
    modules,
    permissions,
    ready,
    setupRequired,
    initialized,
    loading,
    version,
    libuvcpp,
    dataDir,
    statusError,
    isAuthenticated,
    isAdmin,
    canAdminUsers,
    canAdminSettings,
    initialize,
    login,
    setup,
    logout,
    refresh,
    clearSession,
    moduleEnabled,
  }
})
