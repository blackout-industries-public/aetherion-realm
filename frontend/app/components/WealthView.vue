<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { T, FONT, fmt } from '../theme'
import UiPanel from './UiPanel.vue'
import UiSpark from './UiSpark.vue'

const emit = defineEmits<{ select: [string] }>()

const { data: wealth, refresh } = useFetch<any>('/api/wealth')

let timer: ReturnType<typeof setInterval> | null = null
onMounted(() => { timer = setInterval(() => refresh(), 45_000) })
onUnmounted(() => { if (timer) clearInterval(timer) })

// Copper to a readable gold string; the economy thinks in copper, people do not.
function gold(copper: number) {
  if (copper >= 10000) return `${Math.floor(copper / 10000).toLocaleString('en-GB')}g`
  if (copper >= 100) return `${Math.floor(copper / 100)}s`
  return `${copper}c`
}

const CLS: Record<number, string> = {
  1: 'warrior', 2: 'paladin', 3: 'hunter', 4: 'rogue', 5: 'priest',
  6: 'death knight', 7: 'shaman', 8: 'mage', 9: 'warlock', 11: 'druid',
}

const lede = computed(() => {
  const w = wealth.value
  if (!w?.supply?.bots) {
    return 'No gold ledger yet. Once the fleet is snapshotted, this page shows ' +
      'who holds the realm’s money and where it moves.'
  }
  const top = w.richest?.[0]
  return `The fleet of ${fmt.int(w.supply.bots)} bots holds ${gold(w.supply.total)}.` +
    (top ? ` The richest, ${top.name}, sits on ${gold(top.money)}.` : '')
})

// SQL caps the top band, so the axis is fixed 0..8; missing bands render as
// zero-height columns instead of silently vanishing from the histogram.
const BUCKET_LABELS = ['<0.5k', '0.5-1k', '1-1.5k', '1.5-2k', '2-2.5k',
  '2.5-3k', '3-3.5k', '3.5-4k', '4k+']
const bucketPoints = computed(() => {
  const got = new Map<number, number>(
    (wealth.value?.buckets ?? []).map((b: any) => [b.band, b.n]))
  return BUCKET_LABELS.map((label, i) => ({ label, value: got.get(i) ?? 0 }))
})

const supplyDelta = computed(() => {
  const pts = wealth.value?.supply?.points ?? []
  if (pts.length < 2) return null
  return pts[pts.length - 1].total - pts[0].total
})

// 24h of snapshots normalised into a fixed viewBox so a stretched polyline
// stands in for a chart library.
const sparkPoints = computed(() => {
  const pts = wealth.value?.supply?.points ?? []
  if (pts.length < 2) return null
  const x0 = pts[0].ts
  const spanX = Math.max(1, pts[pts.length - 1].ts - x0)
  const ys = pts.map((p: any) => p.total)
  const yMin = Math.min(...ys)
  const spanY = Math.max(1, Math.max(...ys) - yMin)
  return pts
    .map((p: any) =>
      `${(((p.ts - x0) / spanX) * 100).toFixed(2)},${(30 - ((p.total - yMin) / spanY) * 28).toFixed(2)}`)
    .join(' ')
})

const sparkTitle = computed(() => {
  const pts = wealth.value?.supply?.points ?? []
  if (pts.length < 2) return ''
  return `24h fleet gold: ${gold(pts[0].total)} at ${fmt.clock(pts[0].ts)} to ` +
    `${gold(pts[pts.length - 1].total)} at ${fmt.clock(pts[pts.length - 1].ts)}`
})

const flows = computed(() => wealth.value?.flows ?? null)
</script>

<template>
  <section :style="{ display: 'grid', gridTemplateRows: 'auto 1fr', gap: '14px', padding: '20px 22px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${T.goldDim}`, paddingLeft: '15px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '17.5px',
        lineHeight: 1.4, color: T.textMid, maxWidth: '64ch',
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(300px, 1fr))', gap: '16px', alignItems: 'start' }">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Richest ten" note="whole fleet; dim names are offline">
          <p
            v-if="!(wealth?.richest ?? []).length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >No characters recorded yet.</p>
          <div
            v-for="(r, i) in wealth?.richest ?? []"
            :key="r.guid"
            :style="{ display: 'grid', gridTemplateColumns: '20px 1fr auto', gap: '9px', padding: '4px 0', alignItems: 'baseline', borderBottom: `1px solid ${T.lineFaint}` }"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? T.goldBright : T.faint }">{{ i + 1 }}</span>
            <button
              :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontSize: '13px', color: r.online ? T.text : T.faint, textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
              :title="r.online ? 'online' : 'offline'"
              @click="emit('select', r.name)"
            >{{ r.name }} <span :style="{ color: T.faint }">lv {{ r.level }} {{ CLS[r.cls] ?? '' }}</span></button>
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: T.goldBright, fontVariantNumeric: 'tabular-nums' }">{{ gold(r.money) }}</span>
          </div>
        </UiPanel>

        <UiPanel cap="Top earners" note="vendor and mail income, 24h">
          <p
            v-if="!(wealth?.earners ?? []).length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >No copper has come in over the last 24h.</p>
          <div
            v-for="(s, i) in wealth?.earners ?? []"
            :key="s.guid"
            :style="{ display: 'grid', gridTemplateColumns: '20px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? T.goldBright : T.faint }">{{ i + 1 }}</span>
            <button
              :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontSize: '13px', color: T.text, textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
              @click="emit('select', s.name)"
            >{{ s.name }} <span :style="{ color: T.faint }">lv {{ s.level }}</span></button>
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: T.goldBright, fontVariantNumeric: 'tabular-nums' }">{{ gold(s.earned) }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">{{ fmt.int(s.sells + s.collects) }} ev</span>
          </div>
        </UiPanel>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Gold supply" :note="supplyDelta !== null ? `${supplyDelta >= 0 ? '+' : '-'}${gold(Math.abs(supplyDelta))} in 24h` : 'history accumulating'">
          <div :style="{ fontFamily: FONT.display, fontSize: '22px', fontWeight: 700, color: T.goldBright }">
            {{ gold(wealth?.supply?.total ?? 0) }}
          </div>
          <div v-if="sparkPoints" :title="sparkTitle">
            <svg
              viewBox="0 0 100 32"
              preserveAspectRatio="none"
              :style="{ display: 'block', width: '100%', height: '36px', marginTop: '8px' }"
            >
              <polyline :points="sparkPoints" fill="none" :stroke="T.goldDim" stroke-width="1.5" vector-effect="non-scaling-stroke" />
            </svg>
            <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '9.5px', color: T.faint }">
              <span>24h ago</span><span>now</span>
            </div>
          </div>
          <p
            v-else
            :style="{ margin: '6px 0 0', fontSize: '12px', color: T.muted, lineHeight: 1.5 }"
          >Not enough band snapshots yet for a sparkline.</p>
        </UiPanel>

        <UiPanel cap="Wealth distribution" note="online bots, 500g buckets">
          <UiSpark :points="bucketPoints" label="bots per bucket" />
        </UiPanel>

        <UiPanel cap="Sinks vs faucets" note="24h, logged flows only">
          <div :style="{ display: 'flex', gap: '14px', flexWrap: 'wrap', fontFamily: FONT.mono, fontSize: '11.5px', fontVariantNumeric: 'tabular-nums' }">
            <span
              :style="{ color: flows?.vendor.copper ? T.green : T.faint }"
              :title="`${fmt.int(flows?.vendor.n ?? 0)} vendor sales`"
            >in from vendors +{{ gold(flows?.vendor.copper ?? 0) }}</span>
            <span
              :style="{ color: flows?.mail.copper ? T.green : T.faint }"
              :title="`${fmt.int(flows?.mail.n ?? 0)} mail collections`"
            >in from mail +{{ gold(flows?.mail.copper ?? 0) }}</span>
            <span
              :style="{ color: flows?.repairs.copper ? T.red : T.faint }"
              :title="`${fmt.int(flows?.repairs.n ?? 0)} repair bills`"
            >out to repairs -{{ gold(flows?.repairs.copper ?? 0) }}</span>
          </div>
          <p :style="{ margin: '6px 0 0', fontSize: '11px', color: T.faint, lineHeight: 1.4 }">
            Repairs are the only logged sink. AH deposits, the house cut and
            training fees are not recorded, so the out side understates spend.
            Mail income includes auction proceeds - transfers, not new copper.
          </p>
        </UiPanel>
      </div>
    </div>
  </section>
</template>
