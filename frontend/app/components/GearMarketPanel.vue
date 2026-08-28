<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { T, FONT, V, fmt } from '../theme'

const emit = defineEmits<{ select: [string] }>()
const { data: gm, refresh } = await useFetch<any>('/api/gearmarket')
let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => { timer = setInterval(() => { refresh() }, 60_000) })
onUnmounted(() => { if (timer) clearInterval(timer) })

const panel = { border: `1px solid ${V.line}`, background: V.panel, boxShadow: V.inset }
const cap = {
  fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
  letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase' as const,
}
const QUALITY = ['oklch(0.62 0 0)', 'oklch(0.86 0.01 100)', 'oklch(0.72 0.17 145)',
  'oklch(0.68 0.13 250)', 'oklch(0.66 0.18 300)', 'oklch(0.74 0.16 60)'] as const

// Silver reads better than "0.6g" for a market whose upgrades cost pennies.
const price = (copper: number) =>
  copper >= 10000 ? `${(copper / 10000).toFixed(1)}g` : `${Math.round(copper / 100)}s`

const g = computed(() => gm.value)
</script>

<template>
  <section v-if="g" :style="panel">
    <header :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', padding: '10px 12px 6px' }">
      <span :style="cap">Gear market</span>
      <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">last 24h</span>
    </header>

    <div :style="{ padding: '0 12px 12px' }">
      <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 14px', marginBottom: '10px' }">
        <div v-for="s in [
          { v: fmt.int(g.want), l: 'bots want an upgrade' },
          { v: `${g.shelf.equippable}/${g.shelf.listings}`, l: 'listings are wearable' },
          { v: fmt.int(g.trips), l: `shopping trips · ${g.tripBots} bots` },
          { v: `${g.crafts.pct}%`, l: 'of crafts are wearable' },
        ]" :key="s.l">
          <div :style="{ fontFamily: FONT.mono, fontSize: '18px', color: V.textHi }">{{ s.v }}</div>
          <div :style="{ fontSize: '11.5px', color: V.muted }">{{ s.l }}</div>
        </div>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '4px' }">
        <div
          v-for="f in [
            { l: 'gear bought', v: g.bought, c: T.green },
            { l: 'bids placed', v: g.bids, c: V.accent },
            { l: 'reagent bids for gear recipes', v: g.matBids, c: V.text },
            { l: 'gear spared from the shredder', v: g.rescued, c: V.muted },
          ]"
          :key="f.l"
          :style="{ display: 'grid', gridTemplateColumns: '1fr auto', gap: '9px', alignItems: 'baseline' }"
        >
          <span :style="{ fontSize: '12.5px', color: f.v ? V.body : V.faint }">{{ f.l }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: f.v ? f.c : V.faint }">{{ f.v || '—' }}</span>
        </div>
      </div>

      <div v-if="g.recent.length" :style="{ marginTop: '9px', display: 'flex', flexDirection: 'column', gap: '2px' }">
        <div v-for="r in g.recent" :key="r.at + r.bot" :style="{ fontSize: '12px', color: V.body, lineHeight: 1.4 }">
          <button
            class="nm"
            :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontFamily: FONT.body, fontSize: '12px', color: V.textHi }"
            @click="emit('select', r.bot)"
          >{{ r.bot }}</button>
          bought <span :style="{ color: QUALITY[Math.min(r.quality, 5)] }">{{ r.item }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint }"> ilvl {{ r.ilvl }}</span>
        </div>
      </div>

      <p :style="{ margin: '10px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.5 }">
        The cheapest fitting listing averages {{ price(g.avgPrice) }}, so nothing is
        priced out — the constraint is what crafters put on the shelf.
      </p>
    </div>
  </section>
</template>

<style scoped>
.nm:hover { text-decoration: underline }
</style>
