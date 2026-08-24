<script setup lang="ts">
import { computed } from 'vue'
import { FONT, V } from '../theme'

type Series = { label: string; color: string; values: number[]; opacity?: number }

const props = withDefaults(defineProps<{
  /** Bottom of the stack first. Later series stack on top and paint behind. */
  series: Series[]
  cap?: string
  /** End-only labels, oldest to newest. A label per point would bury the shape. */
  range?: [string, string]
  height?: number
}>(), { range: () => ['24h', 'now'] as [string, string], height: 62 })

const layers = computed(() => {
  const n = Math.max(0, ...props.series.map(s => s.values.length))
  if (n < 2) return []
  const totals = Array.from({ length: n }, (_, i) =>
    props.series.reduce((sum, s) => sum + (s.values[i] ?? 0), 0))
  const ymax = Math.max(1, ...totals)
  const step = 960 / (n - 1)
  const cum = Array<number>(n).fill(0)
  const shapes = props.series.map((s, si) => {
    const line = Array.from({ length: n }, (_, i) => {
      cum[i]! += s.values[i] ?? 0
      return `L${(i * step).toFixed(1)},${(88 - (cum[i]! / ymax) * 80).toFixed(1)}`
    }).join(' ')
    return {
      color: s.color,
      opacity: s.opacity ?? Math.max(0.6, 0.85 - si * 0.05),
      d: `M0,88 ${line} L960,88 Z`,
    }
  })
  // Largest cumulative area first, so every layer stays visible in front of it.
  return shapes.reverse()
})
</script>

<template>
  <div>
    <div :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', marginBottom: '5px', gap: '10px' }">
      <span
        v-if="cap"
        :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9px', letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase' }"
      >{{ cap }}</span>
      <span :style="{ display: 'flex', gap: '14px', alignItems: 'baseline', marginLeft: 'auto' }">
        <span
          v-for="s in series"
          :key="s.label"
          :style="{ display: 'flex', alignItems: 'center', gap: '5px' }"
        >
          <span :style="{ width: '7px', height: '7px', background: s.color }" />
          <span :style="{ fontSize: '11.5px', color: V.muted }">{{ s.label }}</span>
        </span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint }">
          {{ range[0] }} → {{ range[1] }}
        </span>
      </span>
    </div>

    <svg
      viewBox="0 0 960 92"
      preserveAspectRatio="none"
      :style="{ display: 'block', width: '100%', height: `${height}px` }"
    >
      <path v-for="(l, i) in layers" :key="i" :d="l.d" :style="{ fill: l.color, opacity: l.opacity }" />
      <line x1="0" y1="88" x2="960" y2="88" :style="{ stroke: V.line }" stroke-width="1" />
    </svg>
  </div>
</template>
