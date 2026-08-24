<script setup lang="ts">
import { FONT, V } from '../theme'

// Level firsts ride above the hairline in the claimant's class colour; boss
// firsts hang below as accent diamonds. One axis, two vocabularies.
defineProps<{
  /** at is 0..1 along the clock. color is the claimant's class colour. */
  marks: { at: number; label: string; color: string; title?: string }[]
  bosses?: { at: number; label: string; title?: string }[]
}>()

const pct = (at: number) => `${Math.min(100, Math.max(0, at * 100)).toFixed(1)}%`
</script>

<template>
  <div :style="{ position: 'relative', height: '66px' }">
    <span :style="{ position: 'absolute', left: 0, right: 0, top: '31px', height: '1px', background: V.line }" />
    <span :style="{ position: 'absolute', left: 0, top: '27px', width: '1px', height: '9px', background: V.pip }" />
    <span :style="{ position: 'absolute', right: 0, top: '27px', width: '1px', height: '9px', background: V.pip }" />

    <span
      v-for="m in marks"
      :key="`${m.label}-${m.at}`"
      :style="{
        position: 'absolute', left: pct(m.at), top: 0, transform: 'translateX(-50%)',
        display: 'flex', flexDirection: 'column', alignItems: 'center', gap: '4px',
      }"
      :title="m.title ?? m.label"
    >
      <span :style="{ fontFamily: FONT.display, fontWeight: 700, fontSize: '11px', color: V.accentBright }">{{ m.label }}</span>
      <span :style="{ width: '9px', height: '9px', borderRadius: '50%', background: m.color, border: `1.5px solid ${V.panelDeep}` }" />
    </span>

    <span
      v-for="b in bosses ?? []"
      :key="`${b.label}-${b.at}`"
      :style="{
        position: 'absolute', left: pct(b.at), top: '37px', transform: 'translateX(-50%)',
        display: 'flex', flexDirection: 'column', alignItems: 'center', gap: '3px',
      }"
      :title="b.title ?? b.label"
    >
      <span :style="{ width: '8px', height: '8px', background: V.accent, transform: 'rotate(45deg)' }" />
      <span :style="{ fontFamily: FONT.mono, fontSize: '9px', color: V.faint }">{{ b.label }}</span>
    </span>
  </div>
</template>
