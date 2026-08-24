<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { FONT, V, fmt } from '../theme'
import { CLASS_COLOR } from '../data'
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

const clsColor = (cls: number | null | undefined) =>
  (cls != null && CLASS_COLOR[cls]) || V.textHi

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
</script>

<template>
  <section :style="{ flex: 'none', padding: '0 22px 18px', display: 'grid', gridTemplateColumns: '1.1fr 1fr 1fr', gap: '16px', alignItems: 'start' }">
    <UiPanel cap="Top earners" note="vendor and mail income, 24h">
      <p
        v-if="!(wealth?.earners ?? []).length"
        :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
      >No copper has come in over the last 24h.</p>
      <div
        v-for="(s, i) in wealth?.earners ?? []"
        :key="s.guid"
        :style="{ display: 'grid', gridTemplateColumns: '18px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
      >
        <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? V.accentBright : V.faint }">{{ i + 1 }}</span>
        <span :style="{ fontSize: '13px', color: V.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
          <button
            class="nm"
            :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontFamily: FONT.body, fontSize: '13px', color: clsColor(s.cls) }"
            @click="emit('select', s.name)"
          >{{ s.name }}</button>
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }"> lv {{ s.level }}</span>
        </span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: V.moneyBright, fontVariantNumeric: 'tabular-nums' }">{{ gold(s.earned) }}</span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint, fontVariantNumeric: 'tabular-nums' }">{{ fmt.int(s.sells + s.collects) }} ev</span>
      </div>
    </UiPanel>

    <UiPanel cap="Gold supply" :note="supplyDelta !== null ? `${supplyDelta >= 0 ? '+' : '-'}${gold(Math.abs(supplyDelta))} in 24h` : 'history accumulating'">
      <div :style="{ fontFamily: FONT.mono, fontSize: '20px', color: V.moneyBright, fontVariantNumeric: 'tabular-nums' }">
        {{ gold(wealth?.supply?.total ?? 0) }}
      </div>
      <div v-if="sparkPoints" :title="sparkTitle">
        <svg
          viewBox="0 0 100 32"
          preserveAspectRatio="none"
          :style="{ display: 'block', width: '100%', height: '36px', marginTop: '8px' }"
        >
          <polyline :points="sparkPoints" fill="none" :stroke="V.moneyDim" stroke-width="1.5" vector-effect="non-scaling-stroke" />
        </svg>
        <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint }">
          <span>24h ago</span><span>now</span>
        </div>
      </div>
      <p
        v-else
        :style="{ margin: '6px 0 0', fontSize: '12px', color: V.muted, lineHeight: 1.5 }"
      >Not enough band snapshots yet for a sparkline.</p>
    </UiPanel>

    <UiPanel cap="Wealth distribution" note="online bots, 500g buckets">
      <UiSpark :points="bucketPoints" :hue="V.moneyDim" label="bots per bucket" />
    </UiPanel>
  </section>
</template>

<style scoped>
.nm:hover { text-decoration: underline }
</style>
