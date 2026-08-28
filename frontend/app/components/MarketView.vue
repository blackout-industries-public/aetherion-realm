<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { FONT, V, fmt } from '../theme'
import GearMarketPanel from './GearMarketPanel.vue'
import { CLASSES, CLASS_COLOR } from '../data'
import UiPanel from './UiPanel.vue'
import UiBars from './UiBars.vue'

const emit = defineEmits<{ select: [string] }>()

// Self-contained: market owns the order book; wealth lends the richest list so
// the whole spec screen renders from one component.
const { data: market, refresh } = useFetch<any>('/api/market', { server: false })
const { data: wealth, refresh: refreshWealth } =
  useFetch<any>('/api/wealth', { server: false })

let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => { timer = setInterval(() => { refresh(); refreshWealth() }, 45_000) })
onUnmounted(() => { if (timer) clearInterval(timer) })

// One decimal at the gold tier so a 2.5g sale is not flattened to 2g; small
// values keep silver/copper tiers so a 44c herb never reads as 0.0g.
function gold(copper: number) {
  if (copper >= 10000) {
    return `${(copper / 10000).toLocaleString('en-GB', {
      minimumFractionDigits: 1, maximumFractionDigits: 1,
    })}g`
  }
  if (copper >= 100) return `${Math.floor(copper / 100)}s`
  return `${copper}c`
}

// Whole gold for holdings and totals, where a decimal would be false precision.
function goldInt(copper: number) {
  if (copper >= 10000) return `${Math.round(copper / 10000).toLocaleString('en-GB')}g`
  if (copper >= 100) return `${Math.floor(copper / 100)}s`
  return `${copper}c`
}

const clsColor = (cls: number | null | undefined) =>
  (cls != null && CLASS_COLOR[cls]) || V.textHi

const strip = computed(() => market.value?.strip ?? null)
const trades = computed(() => market.value?.trades ?? [])
const depth = computed(() => market.value?.depth ?? [])
const hot = computed(() => market.value?.hot ?? [])
const bigSales = computed(() => market.value?.bigSales ?? [])
const bigAsks = computed(() => market.value?.bigAsks ?? [])
const richest = computed(() => wealth.value?.richest ?? [])

// Real sale prices when any sale completed in 24h; otherwise the panel shows
// live buyouts and its caption says so.
const salesMode = computed(() => bigSales.value.length > 0)

const totalAskValue = computed(() =>
  depth.value.reduce((a: number, d: any) => a + d.totalValue, 0))

const stripCells = computed(() => [
  { v: strip.value ? fmt.int(strip.value.listings) : '-', label: 'live listings', hi: true },
  { v: strip.value ? fmt.int(strip.value.sellers) : '-', label: 'distinct sellers' },
  { v: strip.value ? fmt.int(strip.value.sold24) : '-', label: 'sold, 24h' },
  { v: strip.value ? fmt.int(strip.value.bought24) : '-', label: 'bought, 24h' },
  { v: totalAskValue.value > 0 ? goldInt(totalAskValue.value) : '-', label: 'total asking value' },
])

const depthRows = computed(() => depth.value.map((d: any) => ({
  label: d.name, value: d.listings,
  note: d.avgBuyout > 0 ? gold(d.avgBuyout) : '',
})))

const lede = computed(() => {
  const s = strip.value
  if (!s) return 'Waiting for the first read of the auction house.'
  if (s.listings === 0) {
    return 'The auction house stands empty. Every listing that ever appears ' +
      'here will have been carried in by a bot.'
  }
  return `The house holds ${fmt.int(s.listings)} listings from ` +
    `${fmt.int(s.sellers)} seller${s.sellers === 1 ? '' : 's'}. ` +
    (s.sold24 === 0
      ? 'Nothing has sold in the last day.'
      : `${fmt.int(s.sold24)} sale${s.sold24 === 1 ? '' : 's'} completed in the last day.`)
})

const nameBtn = (color: string) => ({
  appearance: 'none', background: 'none', border: 'none', padding: '0',
  cursor: 'pointer', fontFamily: FONT.body, fontSize: '12.5px', color,
})
</script>

<template>
  <section :style="{ padding: '18px 22px', display: 'flex', flexDirection: 'column', gap: '16px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <GearMarketPanel @select="emit('select', $event)" />
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${V.accentDim}`, paddingLeft: '13px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '16px',
        lineHeight: 1.4, color: V.textMid,
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'flex', gap: '26px', flexWrap: 'wrap' }">
      <div v-for="c in stripCells" :key="c.label">
        <div :style="{ fontFamily: FONT.mono, fontSize: '20px', color: c.hi ? V.accentBright : V.textHi, fontVariantNumeric: 'tabular-nums' }">
          {{ c.v }}
        </div>
        <div :style="{ fontFamily: FONT.mono, fontSize: '10px', letterSpacing: '.08em', color: V.faint }">{{ c.label }}</div>
      </div>
    </div>

    <div :style="{ display: 'grid', gridTemplateColumns: '1.1fr 1fr 1fr', gap: '16px', alignItems: 'start' }">
      <UiPanel cap="Recent trades" note="completed sales, newest first">
        <p
          v-if="!trades.length"
          :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
        >No auction has ever completed. The feed begins with the first sale.</p>
        <div
          v-for="(t, i) in trades"
          :key="`${t.at}-${i}`"
          :style="{ display: 'grid', gridTemplateColumns: '32px 1fr auto', gap: '9px', padding: '5px 0', alignItems: 'baseline', borderBottom: `1px solid ${V.lineFaint}` }"
          :title="`${t.seller} sold ${t.count}x ${t.item}${t.buyer ? ` to ${t.buyer}` : ''}${t.price != null ? ` for ${gold(t.price)}` : ''}`"
        >
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">{{ fmt.ago(t.at) }}</span>
          <span :style="{ fontSize: '12.5px', color: V.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
            <button class="nm" :style="nameBtn(clsColor(t.sellerCls))" @click="emit('select', t.seller)">{{ t.seller }}</button>
            <span :style="{ color: V.faint }"> sold {{ t.count > 1 ? `${t.count}× ` : '' }}{{ t.item }}</span>
            <template v-if="t.buyer">
              <span :style="{ color: V.faint }"> to </span>
              <button class="nm" :style="nameBtn(clsColor(t.buyerCls))" @click="emit('select', t.buyer)">{{ t.buyer }}</button>
            </template>
          </span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.moneyBright, fontVariantNumeric: 'tabular-nums' }">
            {{ t.price != null ? gold(t.price) : '' }}
          </span>
        </div>
        <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
          Every sale here was posted, priced and bought by bots with no one watching.
        </p>
      </UiPanel>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Market depth" note="listings per class · mean buyout">
          <p
            v-if="!depthRows.length"
            :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
          >No live listings to measure.</p>
          <UiBars v-else :rows="depthRows" :hue="V.accentBar" label-width="96px" />
        </UiPanel>

        <UiPanel cap="Hot items" note="most-listed, live">
          <p
            v-if="!hot.length"
            :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
          >Nothing is listed right now.</p>
          <div
            v-for="(h, i) in hot"
            :key="h.name"
            :style="{ display: 'grid', gridTemplateColumns: '18px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
            :title="`${h.listings} listings, ${h.units} units; buyout from ${gold(h.minBuyout)}, avg ${gold(h.avgBuyout)}`"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? V.accentBright : V.faint }">{{ i + 1 }}</span>
            <span :style="{ fontSize: '12.5px', color: V.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ h.name }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.textHi, fontVariantNumeric: 'tabular-nums' }">{{ fmt.int(h.listings) }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint, minWidth: '64px', textAlign: 'right', fontVariantNumeric: 'tabular-nums' }">
              {{ h.minBuyout > 0 ? `from ${gold(h.minBuyout)}` : 'bid only' }}
            </span>
          </div>
        </UiPanel>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel
          :cap="salesMode ? 'Biggest sales' : 'Biggest asking prices'"
          :note="salesMode ? '24h, realised prices' : 'no sale in 24h; live buyouts shown'"
        >
          <template v-if="salesMode">
            <div
              v-for="(s, i) in bigSales"
              :key="`${s.at}-${i}`"
              :style="{ display: 'grid', gridTemplateColumns: '18px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
              :title="`${s.seller} sold ${s.count}x ${s.item} for ${s.price != null ? gold(s.price) : 'an unrecorded price'}`"
            >
              <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? V.accentBright : V.faint }">{{ i + 1 }}</span>
              <span :style="{ fontSize: '12.5px', color: V.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
                <button class="nm" :style="nameBtn(clsColor(s.sellerCls))" @click="emit('select', s.seller)">{{ s.seller }}</button>
                <span :style="{ color: V.faint }"> · </span>{{ s.count > 1 ? `${s.count}× ` : '' }}{{ s.item }}
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.moneyBright, fontVariantNumeric: 'tabular-nums' }">{{ s.price != null ? gold(s.price) : '' }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">{{ fmt.ago(s.at) }}</span>
            </div>
          </template>
          <template v-else>
            <p
              v-if="!bigAsks.length"
              :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
            >Nothing sold and nothing is listed with a buyout.</p>
            <div
              v-for="(a, i) in bigAsks"
              :key="`${a.seller}-${i}`"
              :style="{ display: 'grid', gridTemplateColumns: '18px 1fr auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
              :title="`${a.seller} asks ${gold(a.buyout)} for ${a.count}x ${a.item}`"
            >
              <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? V.accentBright : V.faint }">{{ i + 1 }}</span>
              <span :style="{ fontSize: '12.5px', color: V.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
                <button class="nm" :style="nameBtn(V.textHi)" @click="emit('select', a.seller)">{{ a.seller }}</button>
                <span :style="{ color: V.faint }"> asks for </span>{{ a.count > 1 ? `${a.count}× ` : '' }}{{ a.item }}
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.moneyDim, fontVariantNumeric: 'tabular-nums' }">{{ gold(a.buyout) }}</span>
            </div>
          </template>
        </UiPanel>

        <UiPanel cap="Richest" note="whole fleet">
          <p
            v-if="!richest.length"
            :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
          >No characters recorded yet.</p>
          <div
            v-for="(r, i) in richest"
            :key="r.guid"
            :style="{ display: 'grid', gridTemplateColumns: '18px 1fr auto', gap: '9px', padding: '4px 0', alignItems: 'baseline', borderBottom: `1px solid ${V.lineFaint}` }"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? V.accentBright : V.faint }">{{ i + 1 }}</span>
            <span :style="{ fontSize: '13px', color: V.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
              <button
                class="nm"
                :style="{ ...nameBtn(V.text), fontSize: '13px' }"
                :title="r.online ? 'online' : 'offline'"
                @click="emit('select', r.name)"
              >{{ r.name }}</button>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }"> lv {{ r.level }} {{ (CLASSES[r.cls] ?? '').toLowerCase() }}</span>
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: V.moneyBright, fontVariantNumeric: 'tabular-nums' }">{{ goldInt(r.money) }}</span>
          </div>
          <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
            Repairs are the only logged sink — the out side of the ledger understates spend.
          </p>
        </UiPanel>
      </div>
    </div>
  </section>
</template>

<style scoped>
.nm:hover { text-decoration: underline }
</style>
