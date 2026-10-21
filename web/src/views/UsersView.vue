<script setup lang="ts">
import { onActivated, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { del, errorMessage, get, patch, post } from '../api'
import type { ActiveSession, AuditEntry, User } from '../types'
import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'

defineOptions({ name: 'UsersView' })

const auth = useAuthStore()
const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
const users = ref<User[]>([])
const loading = ref(true)
const modalOpen = ref(false)
const editing = ref<User | null>(null)
const form = ref({ username: '', password: '', role: 'viewer' })
const formError = ref('')
const saving = ref(false)
const resetFor = ref<User | null>(null)
const resetPassword = ref('')
const sessions = ref<ActiveSession[]>([])
const sessionsLoading = ref(false)
const audit = ref<AuditEntry[]>([])
const auditLoading = ref(false)

async function load() {
  loading.value = true
  try {
    const response = await get<{ users: User[] }>('/api/users')
    users.value = response.users
  } catch (err) {
    ui.toastError(err, t('users.error.load'))
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
    formError.value = t('users.error.usernameShort')
    return
  }
  if (form.value.password.length < 8) {
    formError.value = t('users.error.passwordShort')
    return
  }
  saving.value = true
  try {
    await post('/api/users', { ...form.value, username: form.value.username.trim() })
    modalOpen.value = false
    await load()
    ui.notify('success', t('users.notify.created'))
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
    ui.notify('success', t('users.notify.roleUpdated'), t('users.notify.roleUpdatedBody', { username: user.username, role }))
  } catch (err) {
    ui.toastError(err, t('users.error.updateRole'))
    await load()
  }
}

async function toggleEnabled(user: User) {
  try {
    await patch(`/api/users/${user.id}`, { enabled: !user.enabled })
    await load()
    ui.notify('success', user.enabled ? t('users.notify.disabled') : t('users.notify.enabled'), user.username)
  } catch (err) {
    ui.toastError(err)
  }
}

async function remove(user: User) {
  if (!window.confirm(t('users.confirm.delete', { username: user.username }))) return
  try {
    await del(`/api/users/${user.id}`)
    await load()
    ui.notify('success', t('users.notify.deleted'))
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
    ui.notify('warning', t('users.notify.passwordShort'))
    return
  }
  try {
    await patch(`/api/users/${resetFor.value.id}`, { password: resetPassword.value })
    ui.notify('success', t('users.notify.reset'), t('users.notify.resetBody', { username: resetFor.value.username }))
    resetFor.value = null
    resetPassword.value = ''
  } catch (err) {
    ui.toastError(err, t('users.error.resetFailed'))
  }
}

async function loadSessions() {
  sessionsLoading.value = true
  try {
    const response = await get<{ sessions: ActiveSession[] }>('/api/auth/sessions')
    sessions.value = response.sessions
  } catch (err) {
    ui.toastError(err, t('users.error.sessions'))
  } finally {
    sessionsLoading.value = false
  }
}

async function loadAudit() {
  auditLoading.value = true
  try {
    const response = await get<{ entries: AuditEntry[] }>('/api/audit?limit=100')
    audit.value = response.entries.slice(-100).reverse()
  } catch (err) {
    ui.toastError(err, t('users.error.audit'))
  } finally {
    auditLoading.value = false
  }
}

async function revokeSession(session: ActiveSession) {
  if (!window.confirm(t('users.sessions.confirm', { username: session.username }))) return
  try {
    await del(`/api/auth/sessions/${session.id}`)
    await loadSessions()
    await loadAudit()
    ui.notify('success', t('users.sessions.revoked'))
  } catch (err) {
    ui.toastError(err, t('users.sessions.revokeFailed'))
  }
}

function initials(name: string) {
  return name.slice(0, 2).toUpperCase()
}

onMounted(() => {
  void load()
  void loadSessions()
  void loadAudit()
})
onActivated(() => {
  void load()
  void loadSessions()
  void loadAudit()
})
</script>

<template>
  <div class="page users-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('users.eyebrow') }}</span>
        <h2>{{ t('users.title') }}</h2>
        <p>{{ t('users.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <button class="button button--primary" @click="openCreate"><Icon name="plus" :size="16" />{{ t('users.new') }}</button>
      </div>
    </header>

    <div class="role-strip">
      <div class="role-card"><span class="role-card__badge role-card__badge--admin"><Icon name="shield" :size="17" /></span><div><strong>{{ t('users.roles.admin.title') }}</strong><p>{{ t('users.roles.admin.text') }}</p></div></div>
      <div class="role-card"><span class="role-card__badge role-card__badge--operator"><Icon name="bolt" :size="17" /></span><div><strong>{{ t('users.roles.operator.title') }}</strong><p>{{ t('users.roles.operator.text') }}</p></div></div>
      <div class="role-card"><span class="role-card__badge role-card__badge--viewer"><Icon name="eye" :size="17" /></span><div><strong>{{ t('users.roles.viewer.title') }}</strong><p>{{ t('users.roles.viewer.text') }}</p></div></div>
    </div>

    <section class="card">
      <header class="card__head">
        <div><span class="eyebrow">{{ t('users.directory.eyebrow') }}</span><h3>{{ t('users.directory.count', { count: users.length }) }}</h3></div>
        <span class="chip mono">SQLite</span>
      </header>
      <div v-if="loading" class="proxy-loading"><span class="spinner" />{{ t('users.loading') }}</div>
      <div v-else class="user-table">
        <div class="user-table__head">
          <span>{{ t('users.table.user') }}</span><span>{{ t('users.table.role') }}</span><span>{{ t('users.table.status') }}</span><span>{{ t('users.table.lastLogin') }}</span><span />
        </div>
        <div v-for="user in users" :key="user.id" class="user-row">
          <div class="user-cell">
            <span class="avatar avatar--table">{{ initials(user.username) }}</span>
            <div><strong>{{ user.username }}<em v-if="user.id === auth.user?.id">{{ t('users.row.you') }}</em></strong><small>{{ t('users.row.created', { id: user.id, date: new Date(user.createdAt).toLocaleDateString() }) }}</small></div>
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
              <i />{{ user.enabled ? t('users.status.enabled') : t('users.status.disabled') }}
            </button>
          </div>
          <div class="user-login">{{ user.lastLogin ? new Date(user.lastLogin).toLocaleString() : t('users.row.never') }}</div>
          <div class="user-actions">
            <button class="icon-button icon-button--sm" :title="t('users.action.resetPassword')" @click="openReset(user)"><Icon name="key" :size="15" /></button>
            <button class="icon-button icon-button--sm icon-button--danger" :title="t('users.action.deleteUser')" :disabled="user.id === auth.user?.id" @click="remove(user)"><Icon name="trash" :size="15" /></button>
          </div>
        </div>
      </div>
    </section>

    <section class="card">
      <header class="card__head">
        <div><span class="eyebrow">{{ t('users.sessions.eyebrow') }}</span><h3>{{ t('users.sessions.title') }}</h3></div>
        <button class="ghost-toggle" @click="loadSessions"><Icon name="refresh" :size="14" />{{ t('users.sessions.refresh') }}</button>
      </header>
      <div v-if="sessionsLoading" class="proxy-loading"><span class="spinner" />{{ t('users.loading') }}</div>
      <div v-else class="user-table">
        <div class="user-table__head">
          <span>{{ t('users.sessions.user') }}</span><span>{{ t('users.sessions.role') }}</span><span>{{ t('users.sessions.created') }}</span><span>{{ t('users.sessions.expires') }}</span><span />
        </div>
        <div v-for="session in sessions" :key="session.id" class="user-row">
          <div class="user-cell"><strong>{{ session.username }}</strong><em v-if="session.current">{{ t('users.sessions.current') }}</em></div>
          <div class="mono">{{ session.role }}</div>
          <div class="user-login">{{ new Date(session.createdAt).toLocaleString() }}</div>
          <div class="user-login">{{ new Date(session.expiresAt * 1000).toLocaleString() }}</div>
          <div class="user-actions">
            <button class="icon-button icon-button--sm icon-button--danger" :title="t('users.sessions.revoke')" @click="revokeSession(session)"><Icon name="close" :size="15" /></button>
          </div>
        </div>
      </div>
    </section>

    <section class="card">
      <header class="card__head">
        <div><span class="eyebrow">{{ t('users.audit.eyebrow') }}</span><h3>{{ t('users.audit.title') }}</h3></div>
        <button class="ghost-toggle" @click="loadAudit"><Icon name="refresh" :size="14" />{{ t('users.audit.refresh') }}</button>
      </header>
      <div v-if="auditLoading" class="proxy-loading"><span class="spinner" />{{ t('users.loading') }}</div>
      <div v-else class="audit-list">
        <div v-for="entry in audit" :key="entry.id" class="audit-row">
          <span class="mono audit-row__time">{{ new Date(entry.at).toLocaleString() }}</span>
          <span class="kind-chip">{{ entry.action }}</span>
          <span class="audit-row__user">{{ entry.username || '—' }}</span>
          <span class="audit-row__target mono">{{ entry.target || '—' }}</span>
          <span class="mono audit-row__ip">{{ entry.ip || '—' }}</span>
        </div>
        <div v-if="!audit.length" class="proxy-rules-empty">{{ t('users.audit.empty') }}</div>
      </div>
    </section>

    <Teleport to="body">
      <Transition name="modal">
        <div v-if="modalOpen" class="modal-scrim" @click.self="modalOpen = false">
          <form class="modal-card" @submit.prevent="createUser">
            <header class="modal-head">
              <div><span class="eyebrow">{{ t('users.modal.eyebrow') }}</span><h3>{{ t('users.modal.createTitle') }}</h3></div>
              <button type="button" class="icon-button" @click="modalOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <label class="field"><span>{{ t('users.modal.username') }}</span><input v-model="form.username" autocomplete="off" :placeholder="t('users.modal.usernamePlaceholder')" autofocus /></label>
              <label class="field"><span>{{ t('users.modal.tempPassword') }}</span><input v-model="form.password" type="password" autocomplete="new-password" :placeholder="t('users.modal.passwordPlaceholder')" /></label>
              <label class="field"><span>{{ t('users.modal.role') }}</span>
                <select v-model="form.role" class="select">
                  <option value="viewer">{{ t('users.modal.roleViewer') }}</option>
                  <option value="operator">{{ t('users.modal.roleOperator') }}</option>
                  <option value="admin">{{ t('users.modal.roleAdmin') }}</option>
                </select>
              </label>
              <div v-if="formError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ formError }}</div>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="modalOpen = false">{{ t('users.modal.cancel') }}</button>
              <button type="submit" class="button button--primary" :disabled="saving"><span v-if="saving" class="spinner" /><Icon v-else name="plus" :size="15" />{{ t('users.modal.create') }}</button>
            </footer>
          </form>
        </div>
      </Transition>
      <Transition name="modal">
        <div v-if="resetFor" class="modal-scrim" @click.self="resetFor = null">
          <form class="modal-card modal-card--sm" @submit.prevent="saveReset">
            <header class="modal-head">
              <div><span class="eyebrow">{{ t('users.reset.eyebrow') }}</span><h3>{{ resetFor.username }}</h3></div>
              <button type="button" class="icon-button" @click="resetFor = null"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <label class="field"><span>{{ t('users.reset.newPassword') }}</span><input v-model="resetPassword" type="password" autocomplete="new-password" :placeholder="t('users.modal.passwordPlaceholder')" autofocus /></label>
              <div class="inline-alert inline-alert--warning"><Icon name="alert" :size="16" />{{ t('users.reset.warning') }}</div>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="resetFor = null">{{ t('users.modal.cancel') }}</button>
              <button type="submit" class="button button--primary"><Icon name="key" :size="15" />{{ t('users.reset.submit') }}</button>
            </footer>
          </form>
        </div>
      </Transition>
    </Teleport>
  </div>
</template>
