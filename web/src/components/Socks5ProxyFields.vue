<script setup lang="ts">
import { useI18nStore } from '../stores/i18n'

defineOptions({ name: 'Socks5ProxyFields' })

interface ProxySettings {
  enabled: boolean
  host: string
  port: number
  username: string
  password: string
}

const props = defineProps<{ modelValue: ProxySettings }>()
const emit = defineEmits<{ 'update:modelValue': [ProxySettings] }>()
const i18n = useI18nStore()
const t = i18n.t

function update(patch: Partial<ProxySettings>) {
  emit('update:modelValue', { ...props.modelValue, ...patch })
}

function toggle(event: Event) {
  update({ enabled: (event.target as HTMLInputElement).checked })
}
</script>

<template>
  <div class="proxy-fields">
    <div class="proxy-fields__head">
      <label class="listener-check">
        <input type="checkbox" :checked="modelValue.enabled" @change="toggle" />
        {{ t('socks5.proxy.enable') }}
      </label>
      <span class="muted">{{ t('socks5.proxy.hint') }}</span>
    </div>
    <div v-if="modelValue.enabled" class="form-grid proxy-fields__grid">
      <label class="field field--grow">
        <span>{{ t('socks5.proxy.host') }}</span>
        <input
          :value="modelValue.host"
          class="mono"
          placeholder="127.0.0.1"
          spellcheck="false"
          @input="update({ host: ($event.target as HTMLInputElement).value })"
        />
      </label>
      <label class="field field--port">
        <span>{{ t('socks5.proxy.port') }}</span>
        <input
          :value="modelValue.port"
          type="number"
          min="1"
          max="65535"
          class="mono"
          @input="update({ port: Number(($event.target as HTMLInputElement).value) })"
        />
      </label>
      <label class="field">
        <span>{{ t('socks5.proxy.username') }}</span>
        <input
          :value="modelValue.username"
          class="mono"
          :placeholder="t('socks5.proxy.usernamePlaceholder')"
          autocomplete="off"
          spellcheck="false"
          @input="update({ username: ($event.target as HTMLInputElement).value })"
        />
      </label>
      <label class="field">
        <span>{{ t('socks5.proxy.password') }}</span>
        <input
          :value="modelValue.password"
          type="password"
          class="mono"
          :placeholder="t('socks5.proxy.passwordPlaceholder')"
          autocomplete="new-password"
          @input="update({ password: ($event.target as HTMLInputElement).value })"
        />
      </label>
    </div>
  </div>
</template>
