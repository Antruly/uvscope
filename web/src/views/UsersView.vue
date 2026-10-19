<script setup lang="ts">
import { onActivated, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { del, errorMessage, get, patch, post } from '../api'
import type { User } from '../types'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'UsersView' })

const auth = useAuthStore()
const ui = useUiStore()
const users = ref<User[]>([])
const loading = ref(true)
const modalOpen = ref(false)
const editing = ref<User | null>(null)
const form = ref({ username: '', password: '', role: 'viewer' })
const formError = ref('')
const saving = ref(false)
const resetFor = ref<User | null>(null)
const resetPassword = ref('')

async function load() {
  loading.value = true
  try {
    const response = await get<{ users: User[] }>('/api/users')
    users.value = response.users
  } catch (err) {
    ui.toastError(err, 'Could not load users')
  } finally {
    loading.value = false
  }
}

function openCreate() {
  form.value = { username: '', password: '', role: 'viewer' }
  formError.value = ''
  modalOpen.value = true
}

async function createUser() {
  formError.value = ''
  if (form.value.username.trim().length < 3) {
    formError.value = 'Username must be at least 3 characters.'
    return
  }
  if (form.value.password.length < 8) {
    formError.value = 'Password must be at least 8 characters.'
    return
  }
  saving.value = true
  try {
    await post('/api/users', { ...form.value, username: form.value.username.trim() })
    modalOpen.value = false
    await load()
    ui.notify('success', 'User created')
  } catch (err) {
    formError.value = errorMessage(err)
  } finally {
    saving.value = false
  }
}

async function changeRole(user: User, event: Event) {
  const role = (event.target as HTMLSelectElement).value
  try {
    await patch(`/api/users/${user.id}`, { role })
    await load()
    ui.notify('success', 'Role updated', `${user.username} is now ${role}`)
  } catch (err) {
    ui.toastError(err, 'Could not update role')
    await load()
  }
}

async function toggleEnabled(user: User) {
  try {
    await patch(`/api/users/${user.id}`, { enabled: !user.enabled })
    await load()
    ui.notify('success', user.enabled ? 'User disabled' : 'User enabled', user.username)
  } catch (err) {
    ui.toastError(err)
  }
}

async function remove(user: User) {
  if (!window.confirm(`Delete user “${user.username}”? This cannot be undone.`)) return
  try {
    await del(`/api/users/${user.id}`)
    await load()
    ui.notify('success', 'User deleted')
  } catch (err) {
    ui.toastError(err)
  }
}

function openReset(user: User) {
  resetFor.value = user
  resetPassword.value = ''
}

async function saveReset() {
  if (!resetFor.value) return
  if (resetPassword.value.length < 8) {
    ui.notify('warning', 'Password must be at least 8 characters')
    return
  }
  try {
    await patch(`/api/users/${resetFor.value.id}`, { password: resetPassword.value })
    ui.notify('success', 'Password reset', `Existing sessions for ${resetFor.value.username} were invalidated.`)
    resetFor.value = null
    resetPassword.value = ''
  } catch (err) {
    ui.toastError(err, 'Password reset failed')
  }
}

function initials(name: string) {
  return name.slice(0, 2).toUpperCase()
}

onMounted(load)
onActivated(load)
</script>

<template>
  <div class="page users-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">Access control</span>
        <h2>People, roles, blast radius.</h2>
        <p>Provision local accounts and keep administrative surfaces limited to the right people.</p>
      </div>
      <div class="page-head__actions">
        <button class="button button--primary" @click="openCreate"><Icon name="plus" :size="16" />New user</button>
      </div>
    </header>

    <div class="role-strip">
      <div class="role-card"><span class="role-card__badge role-card__badge--admin"><Icon name="shield" :size="17" /></span><div><strong>Administrator</strong><p>Users, settings, reset, every debug module and gateway.</p></div></div>
      <div class="role-card"><span class="role-card__badge role-card__badge--operator"><Icon name="bolt" :size="17" /></span><div><strong>Operator</strong><p>Debug modules, history and reverse proxy management.</p></div></div>
      <div class="role-card"><span class="role-card__badge role-card__badge--viewer"><Icon name="eye" :size="17" /></span><div><strong>Viewer</strong><p>Read-only dashboard, history and about pages.</p></div></div>
    </div>

    <section class="card">
      <header class="card__head">
        <div><span class="eyebrow">Directory</span><h3>{{ users.length }} local accounts</h3></div>
        <span class="chip mono">SQLite</span>
      </header>
      <div v-if="loading" class="proxy-loading"><span class="spinner" />Loading users…</div>
      <div v-else class="user-table">
        <div class="user-table__head">
          <span>User</span><span>Role</span><span>Status</span><span>Last login</span><span />
        </div>
        <div v-for="user in users" :key="user.id" class="user-row">
          <div class="user-cell">
            <span class="avatar avatar--table">{{ initials(user.username) }}</span>
            <div><strong>{{ user.username }}<em v-if="user.id === auth.user?.id">you</em></strong><small>#{{ user.id }} · created {{ new Date(user.createdAt).toLocaleDateString() }}</small></div>
          </div>
          <div>
            <select class="select select--compact" :value="user.role" :disabled="user.id === auth.user?.id" @change="changeRole(user, $event)">
              <option value="admin">admin</option>
              <option value="operator">operator</option>
              <option value="viewer">viewer</option>
            </select>
          </div>
          <div>
            <button class="state-toggle" :class="{ 'state-toggle--on': user.enabled }" :disabled="user.id === auth.user?.id" @click="toggleEnabled(user)">
              <i />{{ user.enabled ? 'enabled' : 'disabled' }}
            </button>
          </div>
          <div class="user-login">{{ user.lastLogin ? new Date(user.lastLogin).toLocaleString() : 'never' }}</div>
          <div class="user-actions">
            <button class="icon-button icon-button--sm" title="Reset password" @click="openReset(user)"><Icon name="key" :size="15" /></button>
            <button class="icon-button icon-button--sm icon-button--danger" title="Delete user" :disabled="user.id === auth.user?.id" @click="remove(user)"><Icon name="trash" :size="15" /></button>
          </div>
        </div>
      </div>
    </section>

    <Teleport to="body">
      <Transition name="modal">
        <div v-if="modalOpen" class="modal-scrim" @click.self="modalOpen = false">
          <form class="modal-card" @submit.prevent="createUser">
            <header class="modal-head">
              <div><span class="eyebrow">Local account</span><h3>Create user</h3></div>
              <button type="button" class="icon-button" @click="modalOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <label class="field"><span>Username</span><input v-model="form.username" autocomplete="off" placeholder="alice" autofocus /></label>
              <label class="field"><span>Temporary password</span><input v-model="form.password" type="password" autocomplete="new-password" placeholder="At least 8 characters" /></label>
              <label class="field"><span>Role</span>
                <select v-model="form.role" class="select">
                  <option value="viewer">Viewer — read only</option>
                  <option value="operator">Operator — debug and gateway</option>
                  <option value="admin">Administrator — full control</option>
                </select>
              </label>
              <div v-if="formError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ formError }}</div>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="modalOpen = false">Cancel</button>
              <button type="submit" class="button button--primary" :disabled="saving"><span v-if="saving" class="spinner" /><Icon v-else name="plus" :size="15" />Create user</button>
            </footer>
          </form>
        </div>
      </Transition>
      <Transition name="modal">
        <div v-if="resetFor" class="modal-scrim" @click.self="resetFor = null">
          <form class="modal-card modal-card--sm" @submit.prevent="saveReset">
            <header class="modal-head">
              <div><span class="eyebrow">Credential reset</span><h3>{{ resetFor.username }}</h3></div>
              <button type="button" class="icon-button" @click="resetFor = null"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <label class="field"><span>New password</span><input v-model="resetPassword" type="password" autocomplete="new-password" placeholder="At least 8 characters" autofocus /></label>
              <div class="inline-alert inline-alert--warning"><Icon name="alert" :size="16" />All existing sessions for this user will be invalidated.</div>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="resetFor = null">Cancel</button>
              <button type="submit" class="button button--primary"><Icon name="key" :size="15" />Reset password</button>
            </footer>
          </form>
        </div>
      </Transition>
    </Teleport>
  </div>
</template>
