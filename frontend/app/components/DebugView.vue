<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { T, FONT, V, fmt } from '../theme'

const { data: dbg, refresh } = await useFetch<any>('/api/debug?hours=24')
let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => { timer = setInterval(() => { refresh() }, 60_000) })
onUnmounted(() => { if (timer) clearInterval(timer) })

const panel = { border: `1px solid ${V.line}`, background: V.panel, boxShadow: V.inset }
const cap = {
  fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
  letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase' as const,
}
const COLS = '1fr 46px 52px 52px 54px 52px 46px 50px 150px'

// Each verdict gets a fixed colour so a venue's dominant failure is readable at a
// glance across the table. Green is the only success; the rest are graded by how
// far the party got before it stopped.
const VERDICT: Record<string, string> = {
  'cleared': T.green,
  'partial': 'oklch(0.80 0.10 88)',
  'ground down': T.red,
  'wiped out': T.red,
  'idle inside': 'oklch(0.70 0.10 300)',
  'died at the door': 'oklch(0.72 0.09 30)',
  'lost on the road': V.muted,
}

const open = ref<number | null>(null)
const venues = computed(() => dbg.value?.venues ?? [])
const totals = computed(() => dbg.value?.totals ?? null)

const verdictRows = computed(() => {
  const v = totals.value?.verdicts ?? {}
  const n = Object.values(v).reduce((a: number, b: any) => a + Number(b), 0) || 1
  return Object.entries(v)
    .map(([k, c]) => ({ k, c: Number(c), pct: Math.round((Number(c) / n) * 100) }))
    .sort((a, b) => b.c - a.c)
})
</script>

<template>
  <section :style="{ padding: '18px 22px', display: 'flex', flexDirection: 'column', gap: '16px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      v-if="totals"
      :style="{
        margin: 0, borderLeft: `2px solid ${V.accentDim}`, paddingLeft: '13px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '15px',
        lineHeight: 1.45, color: V.textMid,
      }"
    >
      {{ totals.runs }} runs finished in the last {{ dbg.hours }} hours. {{ totals.cleared }}
      cleared, {{ totals.scored }} killed something, {{ totals.bosses }} bosses down.
    </p>

    <section v-if="verdictRows.length" :style="panel">
      <header :style="{ padding: '10px 12px 6px' }"><span :style="cap">Where runs end</span></header>
      <div :style="{ padding: '0 12px 12px', display: 'flex', flexDirection: 'column', gap: '4px' }">
        <div
          v-for="r in verdictRows"
          :key="r.k"
          :style="{ display: 'grid', gridTemplateColumns: '150px 1fr 62px', gap: '10px', alignItems: 'center' }"
        >
          <span :style="{ fontSize: '12.5px', color: VERDICT[r.k] ?? V.body }">{{ r.k }}</span>
          <span :style="{ height: '7px', background: V.track, position: 'relative' }">
            <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: `${r.pct}%`, background: VERDICT[r.k] ?? V.muted }" />
          </span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.text }">{{ r.c }} · {{ r.pct }}%</span>
        </div>
      </div>
    </section>

    <section :style="panel">
      <header :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', padding: '10px 14px 8px' }">
        <span :style="cap">Venue by venue</span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">click a row for the breakdown</span>
      </header>

      <div
        :style="{
          display: 'grid', gridTemplateColumns: COLS, gap: '10px', padding: '0 14px 8px',
          borderBottom: `1px solid ${V.line}`, fontFamily: FONT.display, fontWeight: 600,
          fontSize: '9px', letterSpacing: '.12em', color: V.faint2,
        }"
      >
        <span>VENUE</span><span>RUNS</span><span>CLEAR</span><span>SCORED</span>
        <span>DEPTH</span><span>DEATHS</span><span>ILVL</span><span>MINS</span><span>MOSTLY</span>
      </div>

      <div v-for="v in venues" :key="v.map">
        <button
          :class="open === v.map ? '' : 'hv-raised'"
          :style="{
            appearance: 'none', width: '100%', textAlign: 'left', border: 'none',
            borderBottom: `1px solid ${V.lineFaint}`, cursor: 'pointer', padding: 0,
            display: 'block', fontFamily: FONT.body,
            ...(open === v.map ? { background: V.raisedHi } : {}),
          }"
          @click="open = open === v.map ? null : v.map"
        >
          <span :style="{ display: 'grid', gridTemplateColumns: COLS, gap: '10px', padding: '7px 14px', alignItems: 'center' }">
            <span :style="{ display: 'flex', alignItems: 'center', gap: '7px', minWidth: 0 }">
              <span :style="{ width: '3px', height: '13px', background: v.isRaid ? V.accent : V.line, flex: 'none' }" />
              <span :style="{ fontSize: '13px', color: V.textHi, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ v.dungeon }}</span>
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.text }">{{ v.runs }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: v.clearPct ? T.green : V.faint }">{{ v.clearPct }}%</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: v.scorePct ? V.text : V.faint }">{{ v.scorePct }}%</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: v.depthPct ? V.accent : V.faint }">{{ v.deepest }}/{{ v.encounters || '?' }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: v.deaths >= 5 ? T.red : V.muted }">{{ v.deaths }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.muted }">{{ v.ilvl || '—' }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.faint }">{{ v.mins }}m</span>
            <span :style="{ fontSize: '11.5px', color: VERDICT[v.worst] ?? V.muted, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
              {{ v.worst }} <span :style="{ color: V.faint }">×{{ v.worstN }}</span>
            </span>
          </span>
        </button>

        <div
          v-if="open === v.map"
          :style="{ padding: '12px 14px 14px 26px', borderBottom: `1px solid ${V.lineFaint}`, background: V.panelOpen, display: 'flex', flexWrap: 'wrap', gap: '26px' }"
        >
          <div :style="{ minWidth: '210px' }">
            <div :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9px', letterSpacing: '.14em', color: V.faint2, marginBottom: '6px' }">EVERY ENDING</div>
            <div
              v-for="(n, k) in v.verdicts"
              :key="k"
              :style="{ display: 'grid', gridTemplateColumns: '1fr auto', gap: '10px', fontSize: '12.5px', lineHeight: 1.6 }"
            >
              <span :style="{ color: VERDICT[k] ?? V.body }">{{ k }}</span>
              <span :style="{ fontFamily: FONT.mono, color: V.text }">{{ n }}</span>
            </div>
            <div v-if="v.interrupted" :style="{ fontSize: '11.5px', color: V.faint, marginTop: '5px' }">
              {{ v.interrupted }} cut short by a restart
            </div>
          </div>

          <div :style="{ minWidth: '210px' }">
            <div :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9px', letterSpacing: '.14em', color: V.faint2, marginBottom: '6px' }">BOSSES DOWN</div>
            <div v-if="v.killed.length" :style="{ fontSize: '12.5px', color: T.green, lineHeight: 1.6 }">
              <div v-for="k in v.killed" :key="k.boss">{{ k.boss }} <span :style="{ color: V.faint, fontFamily: FONT.mono }">×{{ k.n }}</span></div>
            </div>
            <div v-else :style="{ fontSize: '12.5px', color: V.muted }">Nothing has died here.</div>
          </div>

          <div :style="{ minWidth: '170px', fontSize: '12.5px', color: V.body, lineHeight: 1.7 }">
            <div :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9px', letterSpacing: '.14em', color: V.faint2, marginBottom: '6px' }">SHAPE</div>
            <div>{{ v.entered }} of {{ v.runs }} got inside</div>
            <div>{{ v.bosses }} bosses across all runs</div>
            <div>{{ v.wipes }} wipes per run</div>
            <div :style="{ color: V.faint }">last run {{ fmt.ago(v.lastAt) }} ago</div>
          </div>
        </div>
      </div>
    </section>
  </section>
</template>
