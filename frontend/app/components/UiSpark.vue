<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT } from '../theme'

type Point = { label: string; value: number; second?: number }

const props = withDefaults(defineProps<{
  points: Point[]
  hue?: string
  /** Optional second series, drawn behind — for paired counts like deaths vs revives. */
  secondHue?: string
  secondLabel?: string
  label?: string
}>(), {
  hue: 'oklch(0.74 0.14 85)',
  secondHue: 'oklch(0.55 0.06 158)',
})

const ceiling = computed(() => Math.max(
  1, ...props.points.map(p => Math.max(p.value, p.second ?? 0))))

// Only the ends get a label. A number on every column is the classic way to make a
// small chart unreadable.
const endLabels = computed(() => {
  const n = props.points.length
  return new Set(n <= 2 ? props.points.map(p => p.label) : [props.points[0]!.label, props.points[n - 1]!.label])
})
</script>

<template>
  <div>
    <div :style="{ display: 'flex', alignItems: 'flex-end', gap: '2px', height: '54px' }">
      <span
        v-for="p in points"
        :key="p.label"
        :style="{ flex: 1, position: 'relative', height: '100%', display: 'flex', alignItems: 'flex-end' }"
        :title="`${p.label} · ${p.value}${p.second !== undefined ? ` / ${p.second}` : ''}`"
      >
        <span
          v-if="p.second !== undefined"
          :style="{
            position: 'absolute', left: 0, right: 0, bottom: 0,
            height: `${(p.second / ceiling) * 100}%`,
            background: secondHue, opacity: 0.55, borderRadius: '2px 2px 0 0',
          }"
        />
        <span
          :style="{
            position: 'relative', width: '100%',
            height: `${(p.value / ceiling) * 100}%`,
            minHeight: p.value > 0 ? '2px' : '0',
            background: hue, borderRadius: '2px 2px 0 0',
          }"
        />
      </span>
    </div>

    <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '5px' }">
      <span
        v-for="p in points.filter(x => endLabels.has(x.label))"
        :key="p.label"
        :style="{ fontFamily: FONT.mono, fontSize: '9.5px', color: T.faint }"
      >{{ p.label }}</span>
    </div>

    <div v-if="label" :style="{ display: 'flex', alignItems: 'center', gap: '12px', marginTop: '6px' }">
      <span :style="{ display: 'flex', alignItems: 'center', gap: '5px' }">
        <span :style="{ width: '7px', height: '7px', background: hue }" />
        <span :style="{ fontSize: '12px', color: T.muted }">{{ label }}</span>
      </span>
      <span v-if="secondLabel" :style="{ display: 'flex', alignItems: 'center', gap: '5px' }">
        <span :style="{ width: '7px', height: '7px', background: secondHue, opacity: 0.55 }" />
        <span :style="{ fontSize: '12px', color: T.muted }">{{ secondLabel }}</span>
      </span>
      <span :style="{ marginLeft: 'auto', fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">
        peak {{ ceiling.toLocaleString('en-GB') }}
      </span>
    </div>
  </div>
</template>
