<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import Icon from '../components/Icon.vue'
import { del, errorMessage, get, patch, post } from '../api'
import { useAuthStore } from '../stores/auth'
import { useI18nStore } from '../stores/i18n'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'CommandLibraryView' })

interface CommandEntry {
  id: number
  command: string
  nameEn: string
  nameZh: string
  descriptionEn: string
  descriptionZh: string
  category: string
  platform: 'linux' | 'windows' | 'both'
  tags: string
  quick: boolean
  builtin: boolean
  ownerId: number
}

const categories = [
  'system', 'cpu-memory', 'disk', 'network', 'process', 'users', 'files',
  'packages', 'services', 'logs', 'security', 'containers', 'development', 'custom',
]

const auth = useAuthStore()
const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
const router = useRouter()

const detectedPlatform = /Windows/i.test(navigator.userAgent) ? 'windows' : 'linux'
const platform = ref<'linux' | 'windows' | ''>(detectedPlatform)
const category = ref('')
const search = ref('')
const commands = ref<CommandEntry[]>([])
const loading = ref(true)
const error = ref('')
const editorOpen = ref(false)
const editingId = ref<number | null>(null)
const busy = ref(false)
const form = ref({
  name: '',
  command: '',
  category: 'custom',
  platform: detectedPlatform as 'linux' | 'windows' | 'both',
  description: '',
  quick: false,
})

const isAdmin = computed(() => auth.user?.role === 'admin')
const canOperate = computed(() => auth.user?.role === 'admin' || auth.user?.role === 'operator')
let searchTimer: number | undefined

function commandName(command: CommandEntry) {
  if (i18n.locale === 'zh-CN') return command.nameZh || command.nameEn || command.command
  return command.nameEn || command.nameZh || command.command
}

function commandDescription(command: CommandEntry) {
  return i18n.locale === 'zh-CN'
    ? command.descriptionZh || command.descriptionEn
    : command.descriptionEn || command.descriptionZh
}

function canEdit(command: CommandEntry) {
  if (!canOperate.value) return false
  return isAdmin.value || (!command.builtin && command.ownerId === auth.user?.id)
}

async function load() {
  loading.value = true
  error.value = ''
  try {
    const params = new URLSearchParams()
    if (platform.value) params.set('platform', platform.value)
    if (category.value) params.set('category', category.value)
    if (search.value.trim()) params.set('q', search.value.trim())
    const response = await get<{ ok: boolean; commands: CommandEntry[] }>(
      `/api/terminal/commands?${params.toString()}`,
    )
    if (response.ok) commands.value = response.commands
  } catch (err) {
    error.value = errorMessage(err)
  } finally {
    loading.value = false
  }
}

function openCreate() {
  editingId.value = null
  form.value = {
    name: '',
    command: '',
    category: 'custom',
    platform: detectedPlatform as 'linux' | 'windows',
    description: '',
    quick: false,
  }
  error.value = ''
  editorOpen.value = true
}

function openEdit(command: CommandEntry) {
  editingId.value = command.id
  form.value = {
    name: commandName(command),
    command: command.command,
    category: command.category,
    platform: command.platform,
    description: commandDescription(command),
    quick: command.quick,
  }
  error.value = ''
  editorOpen.value = true
}

async function save() {
  if (!form.value.command.trim() || !form.value.name.trim()) {
    error.value = t('terminal.commands.required')
    return
  }
  busy.value = true
  error.value = ''
  try {
    const body = {
      name: form.value.name.trim(),
      command: form.value.command.trim(),
      category: form.value.category || 'custom',
      platform: form.value.platform,
      description: form.value.description.trim(),
      quick: form.value.quick,
    }
    const response = editingId.value == null
      ? await post<{ ok: boolean; error?: string }>('/api/terminal/commands', body)
      : await patch<{ ok: boolean; error?: string }>(`/api/terminal/commands/${editingId.value}`, body)
    if (!response.ok) {
      error.value = response.error || t('terminal.commands.saveFailed')
      return
    }
    editorOpen.value = false
    await load()
    ui.notify('success', editingId.value == null ? t('terminal.commands.created') : t('terminal.commands.updated'))
  } catch (err) {
    error.value = errorMessage(err)
  } finally {
    busy.value = false
  }
}

async function remove(command: CommandEntry) {
  if (!window.confirm(t('terminal.commands.deleteConfirm', { name: commandName(command) }))) return
  try {
    const response = await del<{ ok: boolean; error?: string }>(
      `/api/terminal/commands/${command.id}`,
    )
    if (!response.ok) {
      ui.notify('error', t('terminal.commands.deleteFailed'), response.error)
      return
    }
    commands.value = commands.value.filter((item) => item.id !== command.id)
    ui.notify('success', t('terminal.commands.deleted'))
  } catch (err) {
    ui.toastError(err, t('terminal.commands.deleteFailed'))
  }
}

async function toggleQuick(command: CommandEntry) {
  try {
    const response = await patch<{ ok: boolean; error?: string }>(
      `/api/terminal/commands/${command.id}`,
      { quick: !command.quick },
    )
    if (!response.ok) {
      ui.notify('error', t('terminal.commands.pinFailed'), response.error)
      return
    }
    command.quick = !command.quick
  } catch (err) {
    ui.toastError(err, t('terminal.commands.pinFailed'))
  }
}

async function copy(command: CommandEntry) {
  try {
    await navigator.clipboard.writeText(command.command)
    ui.notify('success', t('terminal.commands.copied'), command.command)
  } catch {
    ui.notify('warning', t('terminal.commands.copyFailed'))
  }
}

function sendToTerminal(command: CommandEntry) {
  try {
    localStorage.setItem('uvscope-terminal-pending-command', command.command)
  } catch {
    // Clipboard fallback below if storage is unavailable.
  }
  void router.push('/terminal')
}

async function restoreDefaults() {
  if (!window.confirm(t('terminal.commands.restoreConfirm'))) return
  try {
    const response = await post<{ ok: boolean; error?: string }>(
      '/api/terminal/commands/restore',
    )
    if (!response.ok) {
      ui.notify('error', t('terminal.commands.restoreFailed'), response.error)
      return
    }
    await load()
    ui.notify('success', t('terminal.commands.restored'))
  } catch (err) {
    ui.toastError(err, t('terminal.commands.restoreFailed'))
  }
}

watch([platform, category], () => void load())
watch(search, () => {
  window.clearTimeout(searchTimer)
  searchTimer = window.setTimeout(() => void load(), 250)
})

onMounted(() => void load())
</script>

<template>
  <div class="page command-library-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('terminal.commands.eyebrow') }}</span>
        <h2>{{ t('terminal.commands.title') }}</h2>
        <p>{{ t('terminal.commands.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <button v-if="isAdmin" class="button button--ghost" @click="restoreDefaults">
          <Icon name="refresh" :size="15" />{{ t('terminal.commands.restore') }}
        </button>
        <button class="button button--primary" :disabled="!canOperate" @click="openCreate">
          <Icon name="plus" :size="15" />{{ t('terminal.commands.add') }}
        </button>
      </div>
    </header>

    <section class="card command-library-toolbar">
      <div class="segmented">
        <button :class="{ active: platform === detectedPlatform }" @click="platform = detectedPlatform">
          {{ detectedPlatform === 'windows' ? 'Windows' : 'Linux / Unix' }}
        </button>
        <button :class="{ active: platform === '' }" @click="platform = ''">{{ t('terminal.commands.allPlatforms') }}</button>
      </div>
      <label class="field field--grow command-search">
        <Icon name="search" :size="15" />
        <input v-model="search" :placeholder="t('terminal.commands.searchPlaceholder')" spellcheck="false" />
      </label>
      <select v-model="category" class="select">
        <option value="">{{ t('terminal.commands.allCategories') }}</option>
        <option v-for="item in categories" :key="item" :value="item">{{ t(`terminal.category.${item}`) }}</option>
      </select>
      <span class="chip mono">{{ commands.length }}</span>
    </section>

    <div v-if="error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ error }}</div>
    <div v-if="loading" class="session-empty"><span class="spinner" /></div>
    <section v-else-if="commands.length" class="command-list">
      <article
        v-for="command in commands"
        :key="command.id"
        class="command-card"
        @click="copy(command)"
      >
        <div class="command-card__head">
          <strong>{{ commandName(command) }}</strong>
          <span class="chip mono">{{ t(`terminal.category.${command.category}`) }}</span>
          <span v-if="command.builtin" class="chip">{{ t('terminal.commands.builtin') }}</span>
          <span v-if="command.quick" class="chip chip--accent"><Icon name="star" :size="11" />{{ t('terminal.commands.quick') }}</span>
        </div>
        <code class="command-card__code mono">{{ command.command }}</code>
        <p v-if="commandDescription(command)" class="command-card__desc">{{ commandDescription(command) }}</p>
        <div class="command-card__actions">
          <button class="button button--sm" @click.stop="copy(command)"><Icon name="copy" :size="13" />{{ t('terminal.commands.copy') }}</button>
          <button class="button button--sm" @click.stop="sendToTerminal(command)"><Icon name="terminal" :size="13" />{{ t('terminal.commands.run') }}</button>
          <button
            v-if="canEdit(command)"
            class="icon-button icon-button--sm"
            :title="command.quick ? t('terminal.commands.unpin') : t('terminal.commands.pin')"
            @click.stop="toggleQuick(command)"
          >
            <Icon name="star" :size="14" />
          </button>
          <button v-if="canEdit(command)" class="icon-button icon-button--sm" :title="t('terminal.commands.edit')" @click.stop="openEdit(command)">
            <Icon name="edit" :size="14" />
          </button>
          <button v-if="canEdit(command)" class="icon-button icon-button--sm icon-button--danger" :title="t('terminal.commands.delete')" @click.stop="remove(command)">
            <Icon name="trash" :size="14" />
          </button>
        </div>
      </article>
    </section>
    <div v-else class="session-empty">
      <Icon name="search" :size="22" />
      <span>{{ t('terminal.commands.empty') }}</span>
    </div>

    <Teleport to="body">
      <Transition name="modal">
        <div v-if="editorOpen" class="modal-scrim" @click.self="editorOpen = false">
          <form class="modal-card modal-card--wide" @submit.prevent="save">
            <header class="modal-head">
              <div><span class="eyebrow">{{ t('terminal.commands.eyebrow') }}</span><h3>{{ editingId == null ? t('terminal.commands.add') : t('terminal.commands.edit') }}</h3></div>
              <button type="button" class="icon-button" @click="editorOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <div class="form-grid">
                <label class="field field--grow"><span>{{ t('terminal.commands.name') }}</span><input v-model="form.name" :placeholder="t('terminal.commands.namePlaceholder')" /></label>
                <label class="field"><span>{{ t('terminal.commands.categoryLabel') }}</span>
                  <select v-model="form.category" class="select">
                    <option v-for="item in categories" :key="item" :value="item">{{ t(`terminal.category.${item}`) }}</option>
                  </select>
                </label>
                <label class="field"><span>{{ t('terminal.commands.platform') }}</span>
                  <select v-model="form.platform" class="select">
                    <option value="linux">Linux / Unix</option>
                    <option value="windows">Windows</option>
                    <option value="both">{{ t('terminal.commands.both') }}</option>
                  </select>
                </label>
              </div>
              <label class="field"><span>{{ t('terminal.commands.command') }}</span><input v-model="form.command" class="mono" placeholder="df -h" spellcheck="false" /></label>
              <label class="field"><span>{{ t('terminal.commands.description') }}</span><textarea v-model="form.description" class="code-input code-input--short" :placeholder="t('terminal.commands.descriptionPlaceholder')" /></label>
              <label class="listener-check"><input v-model="form.quick" type="checkbox" />{{ t('terminal.commands.pin') }}</label>
              <div v-if="error" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ error }}</div>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="editorOpen = false">{{ t('terminal.commands.cancel') }}</button>
              <button type="submit" class="button button--primary" :disabled="busy">
                <span v-if="busy" class="spinner" /><Icon v-else name="plus" :size="15" />{{ t('terminal.commands.save') }}
              </button>
            </footer>
          </form>
        </div>
      </Transition>
    </Teleport>
  </div>
</template>
