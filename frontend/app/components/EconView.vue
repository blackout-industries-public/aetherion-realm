<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, V, fmt } from '../theme'
import { CLASS_COLOR } from '../data'
import UiPanel from './UiPanel.vue'
import UiDiverge from './UiDiverge.vue'

const props = defineProps<{ econ: any | null }>()
const emit = defineEmits<{ select: [string] }>()

// Copper to a readable gold string; the economy thinks in copper, people do not.
function gold(copper: number) {
  if (copper >= 10000) return `${Math.floor(copper / 10000).toLocaleString('en-GB')}g`
  if (copper >= 100) return `${Math.floor(copper / 100)}s`
  return `${copper}c`
}

// One decimal past the gold mark for rates and medians, where the fraction matters.
function goldFine(copper: number) {
  if (copper >= 10000)
    return `${(copper / 10000).toLocaleString('en-GB', { minimumFractionDigits: 1, maximumFractionDigits: 1 })}g`
  if (copper >= 100) return `${Math.floor(copper / 100)}s`
  return `${Math.round(copper)}c`
}

const NEED_LABEL: Record<string, string> = {
  repair: 'repairs', training: 'training', mount: 'a mount',
  gear: 'better gear', ammo: 'ammunition', materials: 'materials',
}
const NEED_ORDER = ['repair', 'training', 'mount', 'ammo', 'materials', 'gear']

// Missing kinds read as zero so the flow renders before any event exists.
function flow(kind: string) {
  return props.econ?.ahFlow?.[kind] ?? { n: 0, items: 0, copper: 0 }
}

// The flow panel's two banks. Only these five kinds carry copper in `detail`;
// everything else the fleet spends (vendor buys, training, the house cut) is
// unlogged, which the caption owns up to.
const sources = computed(() => [
  { label: 'vendor sales', copper: flow('vendor_sell').copper },
  { label: 'auctions sold', copper: flow('ah_sold').copper },
  { label: 'mail collected', copper: flow('mail_money').copper },
].sort((a, b) => b.copper - a.copper))

const sinks = computed(() => [
  { label: 'repairs', copper: flow('repair_paid').copper },
  { label: 'auction buys', copper: flow('ah_bought').copper },
].sort((a, b) => b.copper - a.copper))

const flowMax = computed(() =>
  Math.max(1, ...sources.value.map(s => s.copper), ...sinks.value.map(s => s.copper)))

// Thickness carries the copper; zero stays invisible rather than faking a bar.
const thickness = (copper: number) =>
  copper > 0 ? `${Math.max(3, Math.round((copper / flowMax.value) * 18))}px` : '0'

const wallet = computed(() => props.econ?.wallet ?? null)

const walletTotal = computed(() => {
  if (wallet.value) return wallet.value.total
  return (props.econ?.goldBands ?? []).reduce((a: number, b: any) => a + b.total, 0)
})

const goldDelta = computed(() => {
  const h = props.econ?.goldHistory ?? []
  if (h.length < 2) return null
  const dayAgo = Date.now() - 86400_000
  const past = h.find((p: any) => p.ts >= dayAgo) ?? h[0]
  return h[h.length - 1].total - past.total
})

const cph = computed(() => props.econ?.income?.copperPerHour ?? null)

const walletTrend = computed(() => {
  const parts: string[] = []
  if (goldDelta.value !== null)
    parts.push(`${goldDelta.value < 0 ? '-' : '+'}${gold(Math.abs(goldDelta.value))} in 24h`)
  if (cph.value !== null)
    parts.push(`${cph.value < 0 ? '-' : '+'}${goldFine(Math.abs(cph.value))}/h now`)
  return parts.join(' · ')
})

const walletTrendColor = computed(() => {
  const v = goldDelta.value ?? cph.value
  if (v == null || v === 0) return V.muted
  return v > 0 ? T.green : T.red
})

// Funded ratio thresholds are semantic, not chrome: green is comfort, gold is
// caution, the dull red is trouble - fixed across palettes like all data colour.
function ratioColor(r: number) {
  if (r >= 0.8) return T.green
  if (r >= 0.4) return 'oklch(0.80 0.10 88)'
  return 'oklch(0.58 0.10 30)'
}

const needRows = computed(() => {
  const rows = (props.econ?.needs ?? []).filter((n: any) => n.type !== 'errand')
  const order = (t: string) => { const i = NEED_ORDER.indexOf(t); return i < 0 ? 99 : i }
  return rows
    .map((n: any) => {
      // Old payloads lack `priced`; gear was the only unpriced type then.
      const priced = n.priced ?? (n.type === 'gear' ? 0 : n.n)
      const ratio = priced > 0 ? n.funded / priced : 0
      return {
        type: n.type, label: NEED_LABEL[n.type] ?? n.type,
        priced, ratio, color: ratioColor(ratio),
        value: priced === 0
          ? `${fmt.int(n.n)} · unpriced`
          : priced === n.n
            ? `${fmt.int(n.funded)}/${fmt.int(n.n)}`
            : `${fmt.int(n.funded)}/${fmt.int(priced)} priced`,
      }
    })
    .sort((a: any, b: any) =>
      (a.priced === 0 ? 1 : 0) - (b.priced === 0 ? 1 : 0) || order(a.type) - order(b.type))
})

const starved = computed(() => props.econ?.starved ?? [])

const bandBars = computed(() => {
  const bands = props.econ?.goldBands ?? []
  const max = Math.max(1, ...bands.map((b: any) => b.total))
  return bands.map((b: any) => ({ ...b, h: Math.max(2, (b.total / max) * 100) }))
})

const bandNote = computed(() => {
  const bands = props.econ?.goldBands ?? []
  if (!bands.length) return 'No supply snapshot yet.'
  const top = bands.reduce((a: any, b: any) => (b.total > a.total ? b : a))
  const lo = Math.max(1, top.band * 10)
  return `Levels ${lo}–${top.band * 10 + 9} hold the most coin: ` +
    `${gold(top.total)} across ${fmt.int(top.n)} wallets.`
})

// Rank-paired, not cause-paired: biggest sink against biggest faucet on one
// shared scale, so the asymmetry between the banks is the first thing read.
const rungs = computed(() => {
  const f = sources.value
  const s = sinks.value
  const len = Math.max(f.length, s.length)
  const out = []
  for (let i = 0; i < len; i++) {
    const sink = s[i]
    const faucet = f[i]
    if (!(sink?.copper || faucet?.copper)) continue
    out.push({
      sink: sink?.copper ?? 0,
      sinkLabel: sink ? `-${gold(sink.copper)}` : '0g',
      faucet: faucet?.copper ?? 0,
      faucetLabel: faucet ? `+${gold(faucet.copper)}` : '0g',
      caption: `${sink?.label ?? 'no more logged sinks'} · ${faucet?.label ?? 'no more logged faucets'}`,
    })
  }
  return out
})

const lede = computed(() => {
  const e = props.econ
  if (!e?.armed) {
    return 'The needs engine is not armed. When it is, this page shows what every ' +
      'bot wants money for - before anything lets them act on it.'
  }
  const unfunded = (e.needs ?? [])
    .filter((n: any) => n.type !== 'errand')
    .reduce((a: number, n: any) =>
      a + Math.max(0, (n.priced ?? (n.type === 'gear' ? 0 : n.n)) - n.funded), 0)
  const head = unfunded > 0
    ? `${fmt.int(unfunded)} needs are unfunded.`
    : 'Every priced need is funded today.'
  const market = e.market?.listings ?? 0
  return `${head} ` + (market === 0
    ? 'The house is empty - by design, until bots stock it themselves.'
    : `The house holds ${fmt.int(market)} listings from ${e.market.owners} seller${e.market.owners === 1 ? '' : 's'}.`)
})
</script>

<template>
  <section :style="{ padding: '18px 22px', display: 'flex', flexDirection: 'column', gap: '16px', minHeight: 0, overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${V.accentDim}`, paddingLeft: '13px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '16px',
        lineHeight: 1.4, color: V.textMid,
      }"
    >{{ lede }}</p>

    <section :style="{ border: `1px solid ${V.line}`, background: V.panel, boxShadow: V.inset, flex: 'none' }">
      <header :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', padding: '10px 14px 4px' }">
        <span :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '10px', letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase' }">Where the coin moves · 24h</span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">bar thickness is copper</span>
      </header>
      <div :style="{ display: 'grid', gridTemplateColumns: '1fr 300px 1fr', gap: 0, alignItems: 'center', padding: '10px 14px 8px' }">
        <div :style="{ display: 'flex', flexDirection: 'column', gap: '9px' }">
          <div
            v-for="f in sources"
            :key="f.label"
            :style="{ display: 'grid', gridTemplateColumns: '130px 1fr 74px', gap: '10px', alignItems: 'center' }"
          >
            <span :style="{ fontSize: '13.5px', color: V.body, textAlign: 'right' }">{{ f.label }}</span>
            <span :style="{ height: thickness(f.copper), background: `linear-gradient(90deg, oklch(0.74 0.085 158 / .25), oklch(0.74 0.085 158 / .7))` }" />
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', fontVariantNumeric: 'tabular-nums', color: f.copper ? T.green : V.faint }">+{{ gold(f.copper) }}</span>
          </div>
        </div>
        <div :style="{ border: `1px solid ${V.lineAccent}`, background: V.raised, boxShadow: V.inset, padding: '16px 18px', textAlign: 'center', margin: '0 18px' }">
          <div :style="{ fontFamily: FONT.display, fontSize: '9px', fontWeight: 600, letterSpacing: '.22em', color: V.dim, textTransform: 'uppercase' }">Fleet wallet</div>
          <div :style="{ fontFamily: FONT.mono, fontSize: '30px', fontVariantNumeric: 'tabular-nums', color: V.moneyBright, marginTop: '6px', lineHeight: 1 }">{{ gold(walletTotal) }}</div>
          <div v-if="walletTrend" :style="{ fontFamily: FONT.mono, fontSize: '11px', fontVariantNumeric: 'tabular-nums', color: walletTrendColor, marginTop: '6px' }">{{ walletTrend }}</div>
          <div v-else :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.faint, marginTop: '6px' }">history accumulating</div>
          <div v-if="wallet" :style="{ fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint, marginTop: '3px' }">{{ fmt.int(wallet.wallets) }} wallets · median {{ goldFine(wallet.median) }}</div>
        </div>
        <div :style="{ display: 'flex', flexDirection: 'column', gap: '9px' }">
          <div
            v-for="s in sinks"
            :key="s.label"
            :style="{ display: 'grid', gridTemplateColumns: '74px 1fr 130px', gap: '10px', alignItems: 'center' }"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', fontVariantNumeric: 'tabular-nums', color: s.copper ? T.red : V.faint, textAlign: 'right' }">-{{ gold(s.copper) }}</span>
            <span :style="{ height: thickness(s.copper), background: `linear-gradient(90deg, oklch(0.62 0.16 22 / .7), oklch(0.62 0.16 22 / .25))` }" />
            <span :style="{ fontSize: '13.5px', color: V.body }">{{ s.label }}</span>
          </div>
        </div>
      </div>
      <p :style="{ margin: 0, padding: '0 14px 12px', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
        Repairs and auction buys are the only logged sinks - vendor purchases,
        training fees and the house cut go unrecorded, so the out side
        understates spend. Mail income includes auction proceeds: transfers, not
        new coin.
      </p>
    </section>

    <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '14px', alignItems: 'start' }">
      <UiPanel cap="What bots need" note="funded = affordable today">
        <p
          v-if="!needRows.length"
          :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
        >No needs recorded yet.</p>
        <template v-for="n in needRows" :key="n.type">
          <div
            v-if="n.priced > 0"
            :style="{ display: 'grid', gridTemplateColumns: '1fr 1.1fr', gap: '10px', alignItems: 'center', padding: '5px 0', borderBottom: `1px solid ${V.lineFaint}` }"
          >
            <span :style="{ fontSize: '13.5px', color: V.text }">{{ n.label }}</span>
            <span :style="{ display: 'flex', alignItems: 'center', gap: '8px' }">
              <span :style="{ flex: 1, height: '6px', background: V.track, position: 'relative' }">
                <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: `${Math.round(n.ratio * 100)}%`, background: n.color }" />
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', fontVariantNumeric: 'tabular-nums', color: V.text, minWidth: '52px', textAlign: 'right', whiteSpace: 'nowrap' }">{{ n.value }}</span>
            </span>
          </div>
          <div
            v-else
            :style="{ display: 'grid', gridTemplateColumns: '1fr auto', gap: '10px', padding: '5px 0' }"
          >
            <span :style="{ fontSize: '13.5px', color: V.text }">{{ n.label }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint }">{{ n.value }}</span>
          </div>
        </template>
      </UiPanel>

      <UiPanel cap="Longest broke" note="the queue the economy must serve">
        <p
          v-if="!starved.length"
          :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
        >Nobody is starved of a funded need right now.</p>
        <div
          v-for="s in starved"
          :key="`${s.guid}-${s.type}-${s.target}`"
          :style="{ display: 'grid', gridTemplateColumns: '1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
        >
          <span :style="{ fontSize: '13px', minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
            <button
              class="nm"
              :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontFamily: FONT.body, fontSize: '13px', color: CLASS_COLOR[s.cls] ?? V.textHi }"
              @click="emit('select', s.name)"
            >{{ s.name }}</button>
            <span :style="{ color: V.faint }"> needs {{ NEED_LABEL[s.type] ?? s.type }}{{ s.type === 'mount' ? ` (level ${s.target})` : '' }}</span>
          </span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11px', fontVariantNumeric: 'tabular-nums', color: V.muted }">{{ gold(s.amount - s.free) }} short</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint }">{{ fmt.ago(s.since) }}</span>
        </div>
        <p v-if="starved.length" :style="{ margin: '8px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.5 }">
          Unfunded the longest. When this list stops moving, the ledger pass is stuck.
        </p>
      </UiPanel>

      <UiPanel cap="Supply by level band" :note="`${gold(walletTotal)} total`">
        <div :style="{ display: 'flex', alignItems: 'flex-end', gap: '2px', height: '64px' }">
          <span
            v-for="b in bandBars"
            :key="b.band"
            :style="{ flex: 1, height: `${b.h}%`, background: V.moneyDim }"
            :title="`levels ${Math.max(1, b.band * 10)}-${b.band * 10 + 9}: ${gold(b.total)} across ${fmt.int(b.n)} bots`"
          />
        </div>
        <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint }">
          <span>level 1</span><span>80</span>
        </div>
        <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.5 }">{{ bandNote }}</p>
      </UiPanel>

      <UiPanel cap="Faucets against sinks · 24h" note="mirrored on one shared scale">
        <p
          v-if="!rungs.length"
          :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
        >No copper has moved through a logged flow in the last 24h.</p>
        <UiDiverge v-else :rungs="rungs" />
        <p :style="{ margin: '6px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
          One shared scale, so the asymmetry IS the message: the fleet earns far
          faster than its logged sinks drain.
        </p>
      </UiPanel>
    </div>
  </section>
</template>

<style scoped>
.nm:hover { text-decoration: underline; }
</style>
