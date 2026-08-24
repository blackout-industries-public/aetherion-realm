<script setup lang="ts">
// Heroic marker. An SVG glyph rather than the U+2620 character, which renders
// as emoji on most platforms. The red is a data colour - heroic means the
// same thing in every palette - and the eye sockets punch through with the
// panel variable so the glyph sits naturally on any chrome.
withDefaults(defineProps<{ size?: number; title?: string }>(), {
  size: 10,
  title: 'Heroic',
})
const RED = 'oklch(0.66 0.19 25)'
</script>

<template>
  <svg
    :width="size" :height="size" viewBox="0 0 12 12"
    :style="{ flex: 'none', verticalAlign: '-1px' }"
    role="img" :aria-label="title"
  ><title>{{ title }}</title>
    <path
      d="M6 .8A4.4 4.4 0 0 0 1.6 5.2c0 1.7.95 3 2.2 3.75v1.25c0 .55.45 1 1 1h2.4c.55 0 1-.45 1-1V8.95c1.25-.75 2.2-2.05 2.2-3.75A4.4 4.4 0 0 0 6 .8z"
      :fill="RED"
    />
    <circle cx="4.3" cy="5.1" r="1.05" :style="{ fill: 'var(--pnl)' }" />
    <circle cx="7.7" cy="5.1" r="1.05" :style="{ fill: 'var(--pnl)' }" />
    <path d="M5.5 7.4h1L6 8.5z" :style="{ fill: 'var(--pnl)' }" />
  </svg>
</template>
