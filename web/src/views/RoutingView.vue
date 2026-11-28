<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { del, errorMessage, get, post, put } from '../api'
import { useI18nStore } from '../stores/i18n'
import { useUiStore } from '../stores/ui'

defineOptions({ name: 'RoutingView' })

interface ProxyTarget {
  protocol: string
  host: string
  port: number
  username: string
}

interface AddressGroupInfo {
  id: string
  name: string
  builtin: boolean
  kind: string
  description?: string
  source?: string
  rangeCount: number
  available: boolean
  complementOf?: string
  cidrs?: string[]
}

interface RouteRuleInfo {
  id?: string
  groupId: string
  action: string
  proxy: ProxyTarget
}

interface RoutingConfigInfo {
  rules: Array<Omit<RouteRuleInfo, 'proxy'> & { proxy?: ProxyTarget }>
  default: { action: string; proxy?: ProxyTarget }
}

interface MatchResult {
  ok: boolean
  address: string
  groups: string[]
  matched: boolean
  groupId: string
  ruleId: string
  action: string
  reason: 'rule' | 'default'
  proxy: ProxyTarget
}

const i18n = useI18nStore()
const ui = useUiStore()
const t = i18n.t

const loading = ref(true)
const saving = ref(false)
const groups = ref<AddressGroupInfo[]>([])
const rules = ref<RouteRuleInfo[]>([])
const defaultAction = ref('direct')
const defaultProxy = ref<ProxyTarget>(emptyProxy())
const geo = ref<{ source?: string; ipv4Ranges?: number; ipv6Ranges?: number }>({})

const editingId = ref('')
const formName = ref('')
const formDescription = ref('')
const formCidrs = ref('')

const matchAddress = ref('8.8.8.8')
const matchResult = ref<MatchResult | null>(null)
const matchError = ref('')

const builtinGroups = computed(() => groups.value.filter((group) => group.builtin))
const customGroups = computed(() => groups.value.filter((group) => !group.builtin))

function emptyProxy(): ProxyTarget {
  return { protocol: 'socks5', host: '', port: 0, username: '' }
}

function groupName(id: string): string {
  return groups.value.find((group) => group.id === id)?.name || id
}

async function load() {
  loading.value = true
  try {
    const data = await get<{
      groups?: AddressGroupInfo[]
      routing?: RoutingConfigInfo
      geo?: { source?: string; ipv4Ranges?: number; ipv6Ranges?: number }
    }>('/api/routing')
    groups.value = data.groups || []
    rules.value = (data.routing?.rules || []).map((rule) => ({
      id: rule.id,
      groupId: rule.groupId,
      action: rule.action,
      proxy: rule.proxy ? { ...emptyProxy(), ...rule.proxy } : emptyProxy(),
    }))
    defaultAction.value = data.routing?.default?.action || 'direct'
    defaultProxy.value = data.routing?.default?.proxy
      ? { ...emptyProxy(), ...data.routing.default.proxy }
      : emptyProxy()
    geo.value = data.geo || {}
  } catch (err) {
    ui.toastError(err, t('routing.error.load'))
  } finally {
    loading.value = false
  }
}

function startCreate() {
  editingId.value = ''
  formName.value = ''
  formDescription.value = ''
  formCidrs.value = ''
}

function startEdit(group: AddressGroupInfo) {
  editingId.value = group.id
  formName.value = group.name
  formDescription.value = group.description || ''
  formCidrs.value = (group.cidrs || []).join('\n')
}

async function saveGroup() {
  const cidrs = formCidrs.value
    .split(/[\s,]+/)
    .map((item) => item.trim())
    .filter(Boolean)
  if (!formName.value.trim() || cidrs.length === 0) {
    ui.notify('warning', t('routing.custom.cidrs'))
    return
  }
  const body = {
    name: formName.value.trim(),
    description: formDescription.value.trim(),
    cidrs,
  }
  saving.value = true
  try {
    if (editingId.value) {
      await put(`/api/routing/groups/${editingId.value}`, body)
    } else {
      await post('/api/routing/groups', body)
    }
    ui.notify('success', t('routing.notify.groupSaved'))
    startCreate()
    await load()
  } catch (err) {
    ui.toastError(err, t('routing.notify.failed'))
  } finally {
    saving.value = false
  }
}

async function removeGroup(group: AddressGroupInfo) {
  if (!window.confirm(t('routing.custom.deleteConfirm'))) return
  try {
    await del(`/api/routing/groups/${group.id}`)
    ui.notify('success', t('routing.notify.groupDeleted'))
    await load()
  } catch (err) {
    ui.toastError(err, t('routing.notify.failed'))
  }
}

function addRule() {
  rules.value.push({
    groupId: groups.value[0]?.id || '',
    action: 'direct',
    proxy: emptyProxy(),
  })
}

function moveRule(index: number, delta: number) {
  const next = index + delta
  if (next < 0 || next >= rules.value.length) return
  const [rule] = rules.value.splice(index, 1)
  rules.value.splice(next, 0, rule)
}

async function saveRules() {
  const payload = {
    rules: rules.value.map((rule) =>
      rule.action === 'proxy'
        ? { id: rule.id, groupId: rule.groupId, action: rule.action, proxy: rule.proxy }
        : { id: rule.id, groupId: rule.groupId, action: 'direct' },
    ),
    default:
      defaultAction.value === 'proxy'
        ? { action: 'proxy', proxy: defaultProxy.value }
        : { action: 'direct' },
  }
  saving.value = true
  try {
    await put('/api/routing/rules', payload)
    ui.notify('success', t('routing.notify.saved'))
    await load()
  } catch (err) {
    ui.toastError(err, t('routing.notify.failed'))
  } finally {
    saving.value = false
  }
}

async function runMatch() {
  matchError.value = ''
  matchResult.value = null
  try {
    matchResult.value = await post<MatchResult>('/api/routing/match', {
      address: matchAddress.value.trim(),
    })
  } catch (err) {
    matchError.value = errorMessage(err)
  }
}

onMounted(load)
</script>

<template>
  <div class="page routing-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('routing.eyebrow') }}</span>
        <h2>{{ t('routing.title') }}</h2>
        <p>{{ t('routing.lead') }}</p>
      </div>
      <span class="chip mono">CIDR · first match</span>
    </header>

    <section class="card routing-builtin">
      <header class="card__head">
        <div>
          <span class="eyebrow">{{ t('routing.builtin.eyebrow') }}</span>
          <h3>{{ t('routing.builtin.title') }}</h3>
        </div>
        <span class="muted">
          {{ t('routing.builtin.hint') }}
          <template v-if="geo.source"> · {{ geo.source }}</template>
        </span>
      </header>
      <div class="routing-group-grid">
        <article v-for="group in builtinGroups" :key="group.id" class="routing-group-card">
          <div class="routing-group-card__head">
            <strong>{{ group.name }}</strong>
            <span class="chip mono">{{ group.id }}</span>
          </div>
          <p class="muted">{{ group.description }}</p>
          <div class="routing-group-card__facts">
            <span v-if="group.kind === 'complement'" class="chip">
              {{ t('routing.builtin.complement', { base: group.complementOf || '' }) }}
            </span>
            <span v-else class="chip">
              {{ t('routing.builtin.ranges', { count: group.rangeCount }) }}
            </span>
            <span v-if="!group.available" class="chip chip--warn">
              {{ t('routing.builtin.unavailable') }}
            </span>
            <span class="chip">read-only</span>
          </div>
        </article>
      </div>
    </section>

    <div class="routing-columns">
      <section class="card routing-custom">
        <header class="card__head">
          <div>
            <span class="eyebrow">{{ t('routing.custom.eyebrow') }}</span>
            <h3>{{ t('routing.custom.title') }}</h3>
          </div>
          <button v-if="editingId" class="button button--ghost" @click="startCreate">
            {{ t('routing.custom.cancel') }}
          </button>
        </header>

        <div v-if="customGroups.length" class="routing-custom-list">
          <article v-for="group in customGroups" :key="group.id" class="routing-custom-row">
            <div>
              <strong>{{ group.name }}</strong>
              <small class="muted mono">{{ group.cidrs?.length || 0 }} CIDR</small>
              <p v-if="group.description" class="muted">{{ group.description }}</p>
            </div>
            <div class="routing-custom-row__actions">
              <button class="icon-button icon-button--sm" :title="t('routing.custom.edit')" @click="startEdit(group)">
                <Icon name="edit" :size="14" />
              </button>
              <button class="icon-button icon-button--sm" :title="t('routing.custom.delete')" @click="removeGroup(group)">
                <Icon name="trash" :size="14" />
              </button>
            </div>
          </article>
        </div>
        <div v-else class="session-empty">
          <Icon name="layers" :size="20" />
          <span>{{ t('routing.custom.empty') }}</span>
        </div>

        <div class="form-grid routing-group-form">
          <label class="field field--grow">
            <span>{{ t('routing.custom.name') }}</span>
            <input v-model="formName" :placeholder="t('routing.custom.namePlaceholder')" spellcheck="false" />
          </label>
          <label class="field field--grow">
            <span>{{ t('routing.custom.description') }}</span>
            <input v-model="formDescription" spellcheck="false" />
          </label>
          <label class="field field--grow">
            <span>{{ t('routing.custom.cidrs') }}</span>
            <textarea
              v-model="formCidrs"
              rows="4"
              class="mono"
              :placeholder="t('routing.custom.cidrsPlaceholder')"
              spellcheck="false"
            />
          </label>
        </div>
        <div class="form-actions">
          <button class="button button--primary" :disabled="saving" @click="saveGroup">
            <Icon :name="editingId ? 'check' : 'plus'" :size="15" />
            {{ editingId ? t('routing.custom.save') : t('routing.custom.create') }}
          </button>
        </div>
      </section>

      <section class="card routing-rules">
        <header class="card__head">
          <div>
            <span class="eyebrow">{{ t('routing.rules.eyebrow') }}</span>
            <h3>{{ t('routing.rules.title') }}</h3>
          </div>
          <button class="button button--ghost" @click="addRule">
            <Icon name="plus" :size="14" />
            {{ t('routing.rules.add') }}
          </button>
        </header>

        <div v-if="rules.length" class="routing-rule-list">
          <article v-for="(rule, index) in rules" :key="index" class="routing-rule">
            <div class="routing-rule__index mono">{{ index + 1 }}</div>
            <div class="routing-rule__fields">
              <label class="field">
                <span>{{ t('routing.rules.group') }}</span>
                <select v-model="rule.groupId">
                  <option v-for="group in groups" :key="group.id" :value="group.id">
                    {{ group.name }}
                  </option>
                </select>
              </label>
              <label class="field">
                <span>{{ t('routing.rules.action') }}</span>
                <select v-model="rule.action">
                  <option value="direct">{{ t('routing.rules.direct') }}</option>
                  <option value="proxy">{{ t('routing.rules.proxy') }}</option>
                </select>
              </label>
              <template v-if="rule.action === 'proxy'">
                <label class="field">
                  <span>{{ t('routing.rules.protocol') }}</span>
                  <select v-model="rule.proxy.protocol">
                    <option value="socks5">SOCKS5</option>
                    <option value="http">HTTP</option>
                  </select>
                </label>
                <label class="field field--grow">
                  <span>{{ t('routing.rules.host') }}</span>
                  <input v-model="rule.proxy.host" class="mono" spellcheck="false" />
                </label>
                <label class="field field--port">
                  <span>{{ t('routing.rules.port') }}</span>
                  <input v-model.number="rule.proxy.port" type="number" min="1" max="65535" class="mono" />
                </label>
                <label class="field">
                  <span>{{ t('routing.rules.username') }}</span>
                  <input v-model="rule.proxy.username" class="mono" spellcheck="false" />
                </label>
              </template>
            </div>
            <div class="routing-rule__actions">
              <button class="icon-button icon-button--sm" :title="t('routing.rules.up')" :disabled="index === 0" @click="moveRule(index, -1)">
                <Icon name="chevron-left" :size="14" />
              </button>
              <button
                class="icon-button icon-button--sm"
                :title="t('routing.rules.down')"
                :disabled="index === rules.length - 1"
                @click="moveRule(index, 1)"
              >
                <Icon name="chevron-right" :size="14" />
              </button>
              <button class="icon-button icon-button--sm" :title="t('routing.rules.remove')" @click="rules.splice(index, 1)">
                <Icon name="trash" :size="14" />
              </button>
            </div>
          </article>
        </div>
        <div v-else class="session-empty">
          <Icon name="route" :size="20" />
          <span>{{ t('routing.rules.empty') }}</span>
        </div>

        <div class="routing-default">
          <label class="field">
            <span>{{ t('routing.rules.default') }}</span>
            <select v-model="defaultAction">
              <option value="direct">{{ t('routing.rules.direct') }}</option>
              <option value="proxy">{{ t('routing.rules.proxy') }}</option>
            </select>
          </label>
          <template v-if="defaultAction === 'proxy'">
            <label class="field">
              <span>{{ t('routing.rules.protocol') }}</span>
              <select v-model="defaultProxy.protocol">
                <option value="socks5">SOCKS5</option>
                <option value="http">HTTP</option>
              </select>
            </label>
            <label class="field field--grow">
              <span>{{ t('routing.rules.host') }}</span>
              <input v-model="defaultProxy.host" class="mono" spellcheck="false" />
            </label>
            <label class="field field--port">
              <span>{{ t('routing.rules.port') }}</span>
              <input v-model.number="defaultProxy.port" type="number" min="1" max="65535" class="mono" />
            </label>
            <label class="field">
              <span>{{ t('routing.rules.username') }}</span>
              <input v-model="defaultProxy.username" class="mono" spellcheck="false" />
            </label>
          </template>
        </div>
        <p class="muted routing-hint">{{ t('routing.rules.credentialsHint') }}</p>
        <div class="form-actions">
          <button class="button button--primary" :disabled="saving" @click="saveRules">
            <Icon name="check" :size="15" />
            {{ t('routing.rules.save') }}
          </button>
        </div>
      </section>
    </div>

    <section class="card routing-match">
      <header class="card__head">
        <div>
          <span class="eyebrow">{{ t('routing.match.eyebrow') }}</span>
          <h3>{{ t('routing.match.title') }}</h3>
        </div>
      </header>
      <div class="routing-match__form">
        <label class="field field--grow">
          <span>IP</span>
          <input
            v-model="matchAddress"
            class="mono"
            :placeholder="t('routing.match.placeholder')"
            spellcheck="false"
            @keyup.enter="runMatch"
          />
        </label>
        <button class="button button--primary" @click="runMatch">
          <Icon name="search" :size="15" />
          {{ t('routing.match.action') }}
        </button>
      </div>
      <div v-if="matchError" class="routing-match__error">{{ matchError }}</div>
      <div v-else-if="matchResult" class="routing-match__result">
        <span class="chip mono">{{ matchResult.address }}</span>
        <span class="chip" :class="{ 'chip--warn': matchResult.action === 'proxy' }">
          {{ matchResult.action === 'proxy' ? t('routing.rules.proxy') : t('routing.rules.direct') }}
        </span>
        <span class="chip">{{ t(`routing.match.reason.${matchResult.reason}`) }}</span>
        <span v-if="matchResult.groupId" class="chip">
          {{ t('routing.match.group') }} · {{ groupName(matchResult.groupId) }}
        </span>
        <span v-if="matchResult.ruleId" class="chip mono">
          {{ t('routing.match.rule') }} · {{ matchResult.ruleId.slice(0, 8) }}
        </span>
        <span v-if="matchResult.action === 'proxy' && matchResult.proxy?.host" class="chip mono">
          {{ matchResult.proxy.protocol }}://{{ matchResult.proxy.host }}:{{ matchResult.proxy.port }}
        </span>
        <span v-if="!matchResult.matched && !matchResult.groupId" class="muted">
          {{ t('routing.match.none') }}
        </span>
      </div>
    </section>
  </div>
</template>
