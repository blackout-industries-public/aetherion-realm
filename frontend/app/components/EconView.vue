<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, fmt } from '../theme'
import UiPanel from './UiPanel.vue'

const props = defineProps<{ econ: any | null }>()
const emit = defineEmits<{ select: [string] }>()

// Copper to a readable gold string; the economy thinks in copper, people do not.
function gold(copper: number) {
  if (copper >= 10000) return `${Math.floor(copper / 10000).toLocaleString('en-GB')}g`
  if (copper >= 100) return `${Math.floor(copper / 100)}s`
  return `${copper}c`
}

const NEED_LABEL: Record<string, string> = {
  repair: 'repairs', training: 'training', mount: 'a mount',
  gear: 'better gear', ammo: 'ammunition',
}

const lede = computed(() => {
  const e = props.econ
  if (!e?.armed) {
    return 'The needs engine is not armed. When it is, this page shows what every ' +
      'bot wants money for - before anything lets them act on it.'
  }
  const starvedTotal = (e.needs ?? [])
    .filter((n: any) => n.type !== 'gear')
    .reduce((a: number, n: any) => a + (n.n - n.funded), 0)
  const market = e.market?.listings ?? 0
  return `${fmt.int(starvedTotal)} needs are currently unfunded. ` +
    (market === 0
      ? 'The auction house is empty - by design, until bots stock it themselves.'
      : `The auction house holds ${fmt.int(market)} listings from ${e.market.owners} seller${e.market.owners === 1 ? '' : 's'}.`)
})

const goldTotal = computed(() => {
  const bands = props.econ?.goldBands ?? []
  return bands.reduce((a: number, b: any) => a + b.total, 0)
})

const goldDelta = computed(() => {
  const h = props.econ?.goldHistory ?? []
  if (h.length < 2) return null
  const dayAgo = Date.now() - 86400_000
  const past = h.find((p: any) => p.ts >= dayAgo) ?? h[0]
  return h[h.length - 1].total - past.total
})

const bandBars = computed(() => {
  const bands = props.econ?.goldBands ?? []
  const max = Math.max(1, ...bands.map((b: any) => b.total))
  return bands.map((b: any) => ({ ...b, h: Math.max(2, (b.total / max) * 100) }))
})

const income = computed(() => props.econ?.income ?? null)

// The sign carries the story: green means the fleet nets copper, red that
// repairs and training drain it faster than vendoring refills it.
const incomeColor = computed(() => {
  const r = income.value?.copperPerHour
  if (r == null || r === 0) return T.muted
  return r > 0 ? T.green : T.red
})

// 24h of per-snapshot fleet totals, normalised into a fixed viewBox so a plain
// stretched polyline stands in for a chart library.
const sparkPoints = computed(() => {
  const pts = income.value?.points ?? []
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

const destroyed24 = computed(() =>
  (props.econ?.events ?? []).find((e: any) => e.kind === 'destroy') ?? null)
const sold24 = computed(() =>
  (props.econ?.events ?? []).find((e: any) => e.kind === 'vendor_sell') ?? null)

// Missing kinds read as zero so the flow row renders before any event exists.
function flow(kind: string) {
  return props.econ?.ahFlow?.[kind] ?? { n: 0, items: 0, copper: 0 }
}
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
      <UiPanel cap="What bots need" note="funded = they could afford it today">
        <p
          v-if="!(econ?.needs ?? []).length"
          :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
        >No needs recorded yet.</p>
        <div
          v-for="n in econ?.needs ?? []"
          :key="n.type"
          :style="{ display: 'grid', gridTemplateColumns: '1fr auto auto', gap: '10px', padding: '6px 0', alignItems: 'baseline', borderBottom: `1px solid ${T.lineFaint}` }"
        >
          <span :style="{ fontSize: '13.5px', color: T.text }">{{ NEED_LABEL[n.type] ?? n.type }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: T.textHi }">{{ fmt.int(n.n) }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: n.type === 'gear' ? T.faint : (n.funded === n.n ? T.green : T.muted) }">
            {{ n.type === 'gear' ? 'unpriced' : `${fmt.int(n.funded)} funded` }}
          </span>
        </div>
      </UiPanel>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Longest broke" note="unfunded the longest; the queue the economy must serve">
          <p
            v-if="!(econ?.starved ?? []).length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >Nobody is starved of a funded need right now.</p>
          <div
            v-for="s in econ?.starved ?? []"
            :key="`${s.guid}-${s.type}-${s.target}`"
            :style="{ display: 'grid', gridTemplateColumns: '1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
          >
            <button
              :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontSize: '13px', color: T.text, textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
              @click="emit('select', s.name)"
            >{{ s.name }} <span :style="{ color: T.faint }">needs {{ NEED_LABEL[s.type] ?? s.type }}{{ s.type === 'mount' ? ` (level ${s.target})` : '' }}</span></button>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: T.muted }">{{ gold(s.amount - s.free) }} short</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">{{ fmt.ago(s.since) }}</span>
          </div>
        </UiPanel>

        <UiPanel cap="Top earners" note="vendor income, 24h">
          <p
            v-if="!(econ?.sellers ?? []).length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >Nobody has sold to a vendor in the last 24h.</p>
          <div
            v-for="(s, i) in econ?.sellers ?? []"
            :key="s.guid"
            :style="{ display: 'grid', gridTemplateColumns: '20px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? T.goldBright : T.faint }">{{ i + 1 }}</span>
            <button
              :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontSize: '13px', color: T.text, textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
              @click="emit('select', s.name)"
            >{{ s.name }} <span :style="{ color: T.faint }">lv {{ s.level }}</span></button>
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: T.goldBright }">{{ gold(s.earned) }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">{{ fmt.int(s.sells) }} sells</span>
          </div>
        </UiPanel>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Income rate" :note="income?.windowMinutes != null ? `over the last ${income.windowMinutes}m` : 'samples accumulating'">
          <div
            v-if="income?.copperPerHour != null"
            :style="{ fontFamily: FONT.display, fontSize: '22px', fontWeight: 700, color: incomeColor }"
          >{{ income.copperPerHour < 0 ? '-' : '+' }}{{ gold(Math.abs(income.copperPerHour)) }}/h</div>
          <p
            v-else
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >Not enough band snapshots yet to measure a rate.</p>
          <svg
            v-if="sparkPoints"
            viewBox="0 0 100 32"
            preserveAspectRatio="none"
            :style="{ display: 'block', width: '100%', height: '36px', marginTop: '8px' }"
          >
            <polyline :points="sparkPoints" fill="none" :stroke="T.goldDim" stroke-width="1.5" vector-effect="non-scaling-stroke" />
          </svg>
          <div
            v-if="sparkPoints"
            :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '9.5px', color: T.faint }"
          >
            <span>24h ago</span><span>now</span>
          </div>
        </UiPanel>

        <UiPanel cap="Gold supply" :note="goldDelta !== null ? `${goldDelta >= 0 ? '+' : ''}${gold(Math.abs(goldDelta))} in 24h` : 'history accumulating'">
          <div :style="{ fontFamily: FONT.display, fontSize: '22px', fontWeight: 700, color: T.goldBright }">
            {{ gold(goldTotal) }}
          </div>
          <div :style="{ display: 'flex', alignItems: 'flex-end', gap: '2px', height: '44px', marginTop: '8px' }">
            <span
              v-for="b in bandBars"
              :key="b.band"
              :style="{ flex: 1, height: `${b.h}%`, background: 'oklch(0.62 0.09 88)', borderRadius: '1px 1px 0 0' }"
              :title="`levels ${b.band * 10}-${b.band * 10 + 9}: ${gold(b.total)} across ${b.n} bots`"
            />
          </div>
          <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '9.5px', color: T.faint }">
            <span>1</span><span>80</span>
          </div>
        </UiPanel>

        <UiPanel cap="Destruction" note="what the economy loses before it can sell it">
          <div :style="{ fontFamily: FONT.mono, fontSize: '13px', color: destroyed24 ? T.red : T.green }">
            {{ destroyed24 ? `${fmt.int(destroyed24.items)} items in 24h` : 'no destruction recorded' }}
          </div>
          <div
            v-for="d in (econ?.destroyed ?? []).slice(0, 6)"
            :key="d.at + d.item"
            :style="{ display: 'flex', justifyContent: 'space-between', gap: '10px', padding: '3px 0', fontSize: '11.5px' }"
          >
            <span :style="{ color: T.faint, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ d.who }} lost {{ d.count }}x {{ d.item }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint, whiteSpace: 'nowrap' }">{{ fmt.ago(d.at) }}</span>
          </div>
        </UiPanel>

        <UiPanel cap="Auction house" :note="econ?.market ? `${fmt.int(econ.market.listings)} live listings from ${econ.market.owners} sellers` : ''">
          <div v-if="sold24" :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.green, marginBottom: '6px' }">
            {{ fmt.int(sold24.items) }} items vendored in 24h
          </div>
          <div :style="{ display: 'flex', gap: '14px', flexWrap: 'wrap', fontFamily: FONT.mono, fontSize: '11px', color: T.muted, marginBottom: '8px' }">
            <span>posted {{ flow('ah_post').n }}</span>
            <span>listed {{ flow('ah_listed').n }}</span>
            <span :style="{ color: flow('ah_sold').n ? T.green : T.faint }">sold {{ flow('ah_sold').n }}</span>
            <span :style="{ color: T.faint }">expired {{ flow('ah_expired').n }}</span>
            <span>collected {{ gold(flow('mail_money').copper) }}</span>
            <span :style="{ color: flow('craft').n ? T.green : T.faint }">crafts {{ flow('craft').n }}</span>
            <span :style="{ color: T.faint }">banked {{ flow('bank_deposit').n }}</span>
            <span :style="{ color: flow('gather_route').n ? T.green : T.faint }">gather trips {{ flow('gather_route').n }}</span>
            <span :style="{ color: flow('mail_collect').n ? T.green : T.faint }">mail runs {{ flow('mail_collect').n }}</span>
          </div>
          <div
            v-for="(l, i) in econ?.ahListings ?? []"
            :key="i"
            :style="{ display: 'grid', gridTemplateColumns: '1fr auto', gap: '10px', padding: '3px 0', fontSize: '11.5px', borderBottom: `1px solid ${T.lineFaint}` }"
          >
            <span :style="{ color: T.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
              <button
                :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', color: T.textHi, fontSize: '11.5px' }"
                @click="emit('select', l.seller)"
              >{{ l.seller }}</button>
              <span :style="{ color: T.faint }"> lists </span>{{ l.count > 1 ? `${l.count}x ` : '' }}{{ l.item }}
            </span>
            <span :style="{ fontFamily: FONT.mono, color: T.goldDim }">{{ gold(l.buyout) }}</span>
          </div>
          <div v-if="(econ?.ahTop?.sellers ?? []).length || (econ?.ahTop?.buyers ?? []).length" :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px', marginTop: '8px', fontSize: '11px' }">
            <div>
              <div :style="{ color: T.faint, marginBottom: '2px' }">top seller</div>
              <span v-if="econ.ahTop.sellers[0]" :style="{ color: T.textHi }">{{ econ.ahTop.sellers[0].name }} <span :style="{ fontFamily: FONT.mono, color: T.green }">{{ gold(econ.ahTop.sellers[0].copper) }}</span></span>
              <span v-else :style="{ color: T.faint }">nobody yet</span>
            </div>
            <div>
              <div :style="{ color: T.faint, marginBottom: '2px' }">top buyer</div>
              <span v-if="econ.ahTop.buyers[0]" :style="{ color: T.textHi }">{{ econ.ahTop.buyers[0].name }} <span :style="{ fontFamily: FONT.mono, color: T.red }">{{ gold(econ.ahTop.buyers[0].copper) }}</span></span>
              <span v-else :style="{ color: T.faint }">nobody yet</span>
            </div>
          </div>
          <p v-if="!(econ?.ahListings ?? []).length" :style="{ margin: '4px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.4 }">
            The house is empty. Every listing that ever appears here was posted
            by a bot that walked in with goods.
          </p>
        </UiPanel>
      </div>
    </div>
  </section>
</template>
