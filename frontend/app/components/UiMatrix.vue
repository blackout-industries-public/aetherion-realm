<script setup lang="ts">
import { computed } from 'vue'

type Group = { label: string; color: string; count: number }

const props = withDefaults(defineProps<{
  /** One entry per activity; cells are minted at count / per and wear the group colour. */
  groups: Group[]
  /** Characters represented by one cell. */
  per?: number
  columns?: number
}>(), { per: 10, columns: 50 })

const gcd = (a: number, b: number): number => (b ? gcd(b, a % b) : a)

// Interleaved with a fixed stride so the grid reads as a mixed population rather
// than sorted blocks - and identically on server and client, unlike a shuffle.
const cells = computed(() => {
  const seq: Group[] = []
  for (const g of props.groups) {
    const n = g.count > 0 ? Math.max(1, Math.round(g.count / props.per)) : 0
    for (let i = 0; i < n; i++) seq.push(g)
  }
  const len = seq.length
  if (!len) return []
  let stride = 83
  while (gcd(stride, len) !== 1) stride++
  return Array.from({ length: len }, (_, i) => seq[(i * stride) % len]!)
})
</script>

<template>
  <div :style="{ display: 'grid', gridTemplateColumns: `repeat(${columns}, 1fr)`, gap: '2px' }">
    <span
      v-for="(c, i) in cells"
      :key="i"
      :style="{ aspectRatio: '1', background: c.color, opacity: 0.9 }"
      :title="`${c.label} · one cell = ${per} characters`"
    />
  </div>
</template>
