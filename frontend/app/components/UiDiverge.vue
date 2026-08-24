<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, V } from '../theme'

type Rung = {
  sink: number; sinkLabel: string
  faucet: number; faucetLabel: string
  caption: string
}

const props = defineProps<{
  /** Mirrored around the centre: sinks grow left in red, faucets right in green. */
  rungs: Rung[]
}>()

// One shared scale on purpose - the asymmetry between the sides is the message.
const ceiling = computed(() =>
  Math.max(1, ...props.rungs.flatMap(r => [r.sink, r.faucet])))

const width = (v: number) => `${Math.max(v > 0 ? 2 : 0, (v / ceiling.value) * 100)}%`
</script>

<template>
  <div>
    <template v-for="r in rungs" :key="r.caption">
      <div
        :style="{
          display: 'grid', gridTemplateColumns: '52px 1fr 1fr 58px', gap: '8px',
          alignItems: 'center', padding: '4px 0',
        }"
        :title="`${r.caption} · ${r.sinkLabel} out, ${r.faucetLabel} in`"
      >
        <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.red, textAlign: 'right', fontVariantNumeric: 'tabular-nums' }">{{ r.sinkLabel }}</span>
        <span :style="{ display: 'flex', justifyContent: 'flex-end' }">
          <span :style="{ width: width(r.sink), height: '10px', background: T.red, opacity: 0.8 }" />
        </span>
        <span>
          <span :style="{ display: 'block', width: width(r.faucet), height: '10px', background: T.green, opacity: 0.85 }" />
        </span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.green, fontVariantNumeric: 'tabular-nums' }">{{ r.faucetLabel }}</span>
      </div>
      <div :style="{ textAlign: 'center', fontSize: '11px', color: V.faint, paddingBottom: '3px' }">{{ r.caption }}</div>
    </template>
  </div>
</template>
