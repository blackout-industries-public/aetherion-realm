<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT } from '../theme'

type Row = { label: string; value: number; note?: string; tone?: string }

const props = withDefaults(defineProps<{
  rows: Row[]
  /** Single hue for magnitude. A multi-hue ramp would invent categories the data lacks. */
  hue?: string
  /** Fixed denominator, when the reader should compare against a cap rather than the max. */
  max?: number
  unit?: string
  labelWidth?: string
}>(), {
  hue: 'oklch(0.72 0.13 88)',
  labelWidth: '112px',
})

const emit = defineEmits<{ pick: [string] }>()

const ceiling = computed(() =>
  props.max ?? Math.max(1, ...props.rows.map(r => r.value)))
</script>

<template>
  <div
    v-for="r in rows"
    :key="r.label"
    :style="{
      display: 'grid',
      gridTemplateColumns: `${labelWidth} minmax(40px, 1fr) auto`,
      gap: '10px', alignItems: 'center', padding: '3px 0',
    }"
  >
    <span
      :style="{ fontSize: '13px', color: T.body, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
      :title="r.label"
    >{{ r.label }}</span>

    <!-- Thin mark on a recessive track; the track is the 100% reference. -->
    <span :style="{ height: '7px', background: 'oklch(0.26 0.02 56)', position: 'relative', borderRadius: '1px' }">
      <span
        :style="{
          position: 'absolute', inset: '0 auto 0 0',
          width: `${Math.max(r.value > 0 ? 2 : 0, (r.value / ceiling) * 100)}%`,
          background: r.tone ?? hue,
          borderRadius: '1px 3px 3px 1px',
        }"
      />
    </span>

    <span :style="{ display: 'flex', alignItems: 'baseline', gap: '6px', justifyContent: 'flex-end' }">
      <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.text, fontVariantNumeric: 'tabular-nums' }">
        {{ r.value.toLocaleString('en-GB') }}<span v-if="unit" :style="{ color: T.faint }">{{ unit }}</span>
      </span>
      <span
        v-if="r.note"
        :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint, minWidth: '38px', textAlign: 'right' }"
      >{{ r.note }}</span>
    </span>
  </div>
</template>
