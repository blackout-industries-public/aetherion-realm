<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { T, FONT, fmt } from '../theme'
import UiPanel from './UiPanel.vue'
import UiBars from './UiBars.vue'

const emit = defineEmits<{ select: [string] }>()

// Self-contained: this view owns its data so the shell needs no wiring for it.
const { data: market, refresh } = useFetch<any>('/api/market', { server: false })

let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => { timer = setInterval(() => { refresh() }, 45_000) })
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

const strip = computed(() => market.value?.strip ?? null)
const trades = computed(() => market.value?.trades ?? [])
const depth = computed(() => market.value?.depth ?? [])
const hot = computed(() => market.value?.hot ?? [])
const bigSales = computed(() => market.value?.bigSales ?? [])
const bigAsks = computed(() => market.value?.bigAsks ?? [])

// Real sale prices when any sale completed in 24h; otherwise the panel shows
// live buyouts and its caption says so.
const salesMode = computed(() => bigSales.value.length > 0)

const totalAskValue = computed(() =>
  depth.value.reduce((a: number, d: any) => a + d.totalValue, 0))

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

const STRIP_CELLS = [
  { key: 'listings', label: 'live listings' },
  { key: 'sellers', label: 'distinct sellers' },
  { key: 'sold24', label: 'sold, 24h' },
  { key: 'bought24', label: 'bought, 24h' },
] as const
</script>

<template>
  <section :style="{ display: 'grid', gridTemplateRows: 'auto auto 1fr', gap: '14px', padding: '20px 22px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${T.goldDim}`, paddingLeft: '15px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '17.5px',
        lineHeight: 1.4, color: T.textMid, maxWidth: '64ch',
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'flex', gap: '26px', flexWrap: 'wrap' }">
      <div v-for="c in STRIP_CELLS" :key="c.key">
        <div :style="{ fontFamily: FONT.display, fontSize: '20px', fontWeight: 700, color: c.key === 'listings' ? T.goldBright : T.textHi, fontVariantNumeric: 'tabular-nums' }">
          {{ strip ? fmt.int(strip[c.key]) : '-' }}
        </div>
        <div :style="{ fontFamily: FONT.mono, fontSize: '10px', letterSpacing: '.08em', color: T.faint }">{{ c.label }}</div>
      </div>
    </div>

    <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(300px, 1fr))', gap: '16px', alignItems: 'start' }">
      <UiPanel cap="Recent trades" note="last completed sales, newest first">
        <p
          v-if="!trades.length"
          :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
        >No auction has ever completed. The feed begins with the first sale.</p>
        <div
          v-for="(t, i) in trades"
          :key="`${t.at}-${i}`"
          :style="{ display: 'grid', gridTemplateColumns: '34px 1fr auto', gap: '9px', padding: '5px 0', alignItems: 'baseline', borderBottom: `1px solid ${T.lineFaint}` }"
          :title="`${t.seller} sold ${t.count}x ${t.item}${t.buyer ? ` to ${t.buyer}` : ''}${t.price != null ? ` for ${gold(t.price)}` : ''}`"
        >
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">{{ fmt.ago(t.at) }}</span>
          <span :style="{ fontSize: '12.5px', color: T.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
            <button
              :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', color: T.textHi, fontSize: '12.5px' }"
              @click="emit('select', t.seller)"
            >{{ t.seller }}</button>
            <span :style="{ color: T.faint }"> sold </span>{{ t.count > 1 ? `${t.count}x ` : '' }}{{ t.item }}
            <template v-if="t.buyer">
              <span :style="{ color: T.faint }"> to </span>
              <button
                :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', color: T.textHi, fontSize: '12.5px' }"
                @click="emit('select', t.buyer)"
              >{{ t.buyer }}</button>
            </template>
          </span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.goldBright, fontVariantNumeric: 'tabular-nums' }">
            {{ t.price != null ? gold(t.price) : '' }}
          </span>
        </div>
      </UiPanel>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Market depth" :note="totalAskValue > 0 ? `${gold(totalAskValue)} total asking value` : ''">
          <p
            v-if="!depthRows.length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >No live listings to measure.</p>
          <UiBars v-else :rows="depthRows" label-width="104px" />
          <div v-if="depthRows.length" :style="{ marginTop: '5px', fontFamily: FONT.mono, fontSize: '9.5px', color: T.faint }">
            listings per item class; note is the mean non-zero buyout
          </div>
        </UiPanel>

        <UiPanel cap="Hot items" note="most-listed, live">
          <p
            v-if="!hot.length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >Nothing is listed right now.</p>
          <div
            v-for="(h, i) in hot"
            :key="h.name"
            :style="{ display: 'grid', gridTemplateColumns: '20px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
            :title="`${h.listings} listings, ${h.units} units; buyout from ${gold(h.minBuyout)}, avg ${gold(h.avgBuyout)}`"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? T.goldBright : T.faint }">{{ i + 1 }}</span>
            <span :style="{ fontSize: '12.5px', color: T.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ h.name }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: T.textHi, fontVariantNumeric: 'tabular-nums' }">{{ fmt.int(h.listings) }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint, fontVariantNumeric: 'tabular-nums' }">
              {{ h.minBuyout > 0 ? `from ${gold(h.minBuyout)}` : 'bid only' }}
            </span>
          </div>
        </UiPanel>
      </div>

      <UiPanel
        :cap="salesMode ? 'Biggest sales' : 'Biggest asking prices'"
        :note="salesMode ? '24h, realised sale prices' : 'no sale in 24h; live buyouts shown instead'"
      >
        <template v-if="salesMode">
          <div
            v-for="(s, i) in bigSales"
            :key="`${s.at}-${i}`"
            :style="{ display: 'grid', gridTemplateColumns: '20px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
            :title="`${s.seller} sold ${s.count}x ${s.item} for ${s.price != null ? gold(s.price) : 'an unrecorded price'}`"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? T.goldBright : T.faint }">{{ i + 1 }}</span>
            <span :style="{ fontSize: '12.5px', color: T.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
              <button
                :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', color: T.textHi, fontSize: '12.5px' }"
                @click="emit('select', s.seller)"
              >{{ s.seller }}</button>
              <span :style="{ color: T.faint }"> · </span>{{ s.count > 1 ? `${s.count}x ` : '' }}{{ s.item }}
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.goldBright, fontVariantNumeric: 'tabular-nums' }">{{ s.price != null ? gold(s.price) : '' }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">{{ fmt.ago(s.at) }}</span>
          </div>
        </template>
        <template v-else>
          <p
            v-if="!bigAsks.length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >Nothing sold and nothing is listed with a buyout.</p>
          <div
            v-for="(a, i) in bigAsks"
            :key="`${a.seller}-${i}`"
            :style="{ display: 'grid', gridTemplateColumns: '20px 1fr auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
            :title="`${a.seller} asks ${gold(a.buyout)} for ${a.count}x ${a.item}`"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? T.goldBright : T.faint }">{{ i + 1 }}</span>
            <span :style="{ fontSize: '12.5px', color: T.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
              <button
                :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', color: T.textHi, fontSize: '12.5px' }"
                @click="emit('select', a.seller)"
              >{{ a.seller }}</button>
              <span :style="{ color: T.faint }"> asks for </span>{{ a.count > 1 ? `${a.count}x ` : '' }}{{ a.item }}
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.goldDim, fontVariantNumeric: 'tabular-nums' }">{{ gold(a.buyout) }}</span>
          </div>
        </template>
      </UiPanel>
    </div>
  </section>
</template>
