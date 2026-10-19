<script setup lang="ts">
import { useUiStore } from '../stores/ui'
import Icon from './Icon.vue'

const ui = useUiStore()
</script>

<template>
  <Teleport to="body">
    <div class="toast-stack" aria-live="polite">
      <TransitionGroup name="toast">
        <div v-for="toast in ui.toasts" :key="toast.id" class="toast" :class="`toast--${toast.kind}`">
          <div class="toast__icon">
            <Icon
              :name="
                toast.kind === 'success'
                  ? 'check'
                  : toast.kind === 'error'
                    ? 'alert'
                    : toast.kind === 'warning'
                      ? 'alert'
                      : 'spark'
              "
              :size="17"
            />
          </div>
          <div class="toast__body">
            <strong>{{ toast.title }}</strong>
            <span v-if="toast.message">{{ toast.message }}</span>
          </div>
          <button class="icon-button icon-button--sm" aria-label="Dismiss" @click="ui.dismiss(toast.id)">
            <Icon name="close" :size="15" />
          </button>
        </div>
      </TransitionGroup>
    </div>
  </Teleport>
</template>
