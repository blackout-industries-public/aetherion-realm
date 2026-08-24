<script setup lang="ts">
import { T, FONT, V } from '../theme'

type Row = { name: string; downed: number; total: number; raid?: boolean }

withDefaults(defineProps<{
  /** One pip per encounter; filled means ever downed. Raid rows wear the accent. */
  rows: Row[]
  columns?: number
}>(), { columns: 2 })

const countColor = (r: Row) => r.raid
  ? (r.downed > 0 ? V.accentBright : V.faint)
  : (r.total > 0 && r.downed === r.total ? T.green : V.muted)
</script>

<template>
  <div :style="{ display: 'grid', gridTemplateColumns: `repeat(${columns}, 1fr)`, gap: '0 34px' }">
    <div
      v-for="r in rows"
      :key="r.name"
      :style="{ display: 'grid', gridTemplateColumns: '128px 1fr auto', gap: '10px', alignItems: 'center', padding: '4px 0' }"
      :title="`${r.name} · ${r.downed}/${r.total} downed`"
    >
      <span
        :style="{
          fontSize: '12.5px', color: r.raid ? V.accentBright : V.body,
          overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }"
      >{{ r.name }}</span>
      <span :style="{ display: 'flex', gap: '3px', flexWrap: 'wrap' }">
        <span
          v-for="i in r.total"
          :key="i"
          :style="i <= r.downed
            ? { width: '8px', height: '8px', background: r.raid ? V.accent : T.green }
            : { width: '8px', height: '8px', border: `1px solid ${V.pip}` }"
        />
      </span>
      <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: countColor(r), fontVariantNumeric: 'tabular-nums' }">
        {{ r.raid ? 'RAID · ' : '' }}{{ r.downed }}/{{ r.total }}
      </span>
    </div>
  </div>
</template>
