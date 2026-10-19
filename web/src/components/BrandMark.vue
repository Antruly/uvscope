<script setup lang="ts">
withDefaults(defineProps<{ size?: number }>(), { size: 30 })
</script>

<template>
  <svg :width="size" :height="size" viewBox="0 0 40 40" fill="none" aria-hidden="true" class="brand-mark">
    <defs>
      <linearGradient id="uvscope-mark-a" x1="5" y1="3" x2="35" y2="37" gradientUnits="userSpaceOnUse">
        <stop stop-color="var(--accent)" />
        <stop offset="1" stop-color="var(--accent-2)" />
      </linearGradient>
    </defs>
    <rect x="1" y="1" width="38" height="38" rx="11" fill="var(--brand-bg)" stroke="var(--brand-border)" />
    <path d="M9 12.5 15.8 28l5.1-11.3" stroke="url(#uvscope-mark-a)" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round" />
    <circle cx="28.4" cy="16.5" r="5.1" stroke="url(#uvscope-mark-a)" stroke-width="2.2" />
    <path d="M24.6 20.2 21 24" stroke="url(#uvscope-mark-a)" stroke-width="2.2" stroke-linecap="round" />
    <circle cx="29.6" cy="15.4" r="1.2" fill="var(--accent)" />
  </svg>
</template>
