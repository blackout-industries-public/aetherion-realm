<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { T, FONT, STATE, type StateKey, V, fmt, spell, titled } from '../theme'
import UiMatrix from './UiMatrix.vue'

const emit = defineEmits<{ select: [name: string]; goto: [tab: string] }>()

const { data: world, refresh: refreshWorld } = await useFetch<any>('/api/world')
const { data: assembler, refresh: refreshAssembler } = await useFetch<any>('/api/assembler')
const { data: econ, refresh: refreshEcon } = await useFetch<any>('/api/econ')
const { data: market, refresh: refreshMarket } = await useFetch<any>('/api/market')
// The errand verdict census (aetherion_needs, need_type='errand' by target) is only
// exposed by /api/pulse; the four composite endpoints carry no per-target breakdown.
const { data: pulse, refresh: refreshPulse } = await useFetch<any>('/api/pulse')

let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => {
  timer = setInterval(() => {
    refreshWorld(); refreshAssembler(); refreshEcon(); refreshMarket(); refreshPulse()
  }, 30_000)
})
onUnmounted(() => { if (timer) clearInterval(timer) })

const entities = computed<any[]>(() => world.value?.entities ?? [])

// WorldView's precedence, minus the poll-delta movement set: this view has no
// consecutive-poll pair to compare, so unsaved walking reads as idle here.
function stateOf(e: any): StateKey {
  if (e.dead) return 'dead'
  if (e.ghost) return 'ghost'
  if (e.instance > 0) return 'instance'
  if (e.trade) return 'working'
  switch (e.rpg) {
    case 5: return 'questing'
    case 1: case 3: return 'grinding'
    case 2: case 6: return 'travelling'
    case 4: case 7: return 'town'
  }
  if (e.travelling) return 'travelling'
  if (e.grouped) return 'grouped'
  return 'idle'
}

const popRows = computed(() => {
  const counts = new Map<StateKey, number>()
  for (const e of entities.value) {
    const st = stateOf(e)
    counts.set(st, (counts.get(st) ?? 0) + 1)
  }
  const rows = (Object.keys(STATE) as StateKey[])
    .map(k => ({ k, label: STATE[k].label, color: STATE[k].color, n: counts.get(k) ?? 0 }))
    .filter(r => r.n > 0)
    .sort((a, b) => b.n - a.n)
  const max = Math.max(1, ...rows.map(r => r.n))
  return rows.map(r => ({ ...r, w: `${Math.max(1, Math.round((r.n / max) * 100))}%` }))
})

const awake = computed(() => entities.value.length)
const humans = computed(() => entities.value.filter(e => !e.bot).length)
const zoneCount = computed(() => new Set(entities.value.map(e => e.zone)).size)

const board = computed<any[]>(() => assembler.value?.board ?? [])
const flight = computed(() => Number(assembler.value?.activeParties ?? 0))
const inside = computed(() => board.value.filter(b => b.tone === 'inside').length)

const conversion = computed(() => {
  const c = assembler.value?.cycle
  return c?.trips ? Math.round((c.entered / c.trips) * 100) : null
})

// Stage colours match the PVE pipeline columns; travelling wears the travel state
// yellow, inside the semantic green, the door its own pink.
const DOOR_PINK = 'oklch(0.82 0.08 350)'

const groupRows = computed(() => board.value.slice(0, 5).map((b: any) => {
  const cleared = b.tone === 'inside' && b.encounters.length > 0 &&
    b.encounters.every((e: any) => e.killed)
  const status =
    b.tone === 'inside' ? `${cleared ? 'CLEARED' : 'INSIDE'} ${b.dwellMins ?? 0}M`
    : b.tone === 'door' ? 'AT THE DOOR'
    : `${String(b.remaining).toUpperCase()} OUT`
  return {
    id: `${b.isRaid ? 'R' : 'G'}-${b.id}`,
    idColor: b.isRaid ? V.accent : V.muted,
    size: b.size,
    dest: b.dest,
    leader: b.leader,
    status,
    stColor: b.tone === 'inside' ? T.green
      : b.tone === 'door' ? DOOR_PINK
      : STATE.travelling.color,
  }
}))

const signedGold = (copper: number, decimals = 1) =>
  `${copper < 0 ? '−' : '+'}${(Math.abs(copper) / 10000).toLocaleString('en-GB', {
    minimumFractionDigits: decimals, maximumFractionDigits: decimals,
  })}g`

const income = computed(() => econ.value?.income ?? null)
const rate = computed(() => {
  const c = income.value?.copperPerHour
  return c == null ? null : `${signedGold(c)}/h`
})
const rateColor = computed(() =>
  (income.value?.copperPerHour ?? 0) < 0 ? T.red : T.green)

const supplyNote = computed(() => {
  const pts = income.value?.points ?? []
  if (pts.length < 2) return 'no supply history yet'
  const last = pts[pts.length - 1].total
  const delta = last - pts[0].total
  return `supply ${fmt.int(Math.round(last / 10000))}g · ${signedGold(delta, 0)} in 24h`
})

// A handful of points draws the same shape as three hundred; the 5-minute samples
// are downsampled so the polyline stays light.
const sparkPoints = computed(() => {
  const pts: any[] = income.value?.points ?? []
  if (pts.length < 2) return ''
  const step = Math.max(1, Math.floor(pts.length / 48))
  const sampled = pts.filter((_, i) => i % step === 0 || i === pts.length - 1)
  const vals = sampled.map(p => Number(p.total))
  const min = Math.min(...vals), max = Math.max(...vals)
  const span = max - min || 1
  return sampled.map((p, i) =>
    `${((i / (sampled.length - 1)) * 100).toFixed(1)},${(26 - ((Number(p.total) - min) / span) * 22).toFixed(1)}`,
  ).join(' ')
})

const bandBars = computed(() => {
  const bands: any[] = econ.value?.goldBands ?? []
  if (!bands.length) return []
  const max = Math.max(1, ...bands.map(b => Number(b.total)))
  return bands.map(b => ({
    band: b.band,
    h: `${Math.max(2, Math.round((Number(b.total) / max) * 100))}%`,
  }))
})

const flow = (kind: string) => Number(econ.value?.ahFlow?.[kind]?.n ?? 0)

const flowChips = computed(() => [
  { text: `posted ${fmt.int(flow('ah_post'))}`, color: V.muted },
  { text: `sold ${fmt.int(flow('ah_sold'))}`, color: T.green },
  { text: `expired ${fmt.int(flow('ah_expired'))}`, color: V.faint },
  { text: `crafts ${fmt.int(flow('craft'))}`, color: T.green },
  { text: `gather trips ${fmt.int(flow('gather_route'))}`, color: V.muted },
  { text: `mail runs ${fmt.int(flow('mail_collect'))}`, color: V.muted },
])

const ERRAND_LABEL: Record<string, string> = { ah: 'auction house' }

const errandRows = computed(() => {
  const rows: any[] = (pulse.value?.errands ?? [])
    .map((e: any) => ({ label: ERRAND_LABEL[e.target] ?? e.target, n: Number(e.n) }))
    .sort((a: any, b: any) => b.n - a.n)
  const max = Math.max(1, ...rows.map(r => r.n))
  return rows.map(r => ({ ...r, w: `${Math.max(1, Math.round((r.n / max) * 100))}%` }))
})

const strip = computed(() => market.value?.strip ?? null)

const lede = computed(() => {
  if (!awake.value) return 'The realm is asleep: nobody is online.'
  const people = humans.value === 1 ? 'one of them a person' : `${spell(humans.value)} of them people`
  const groups = flight.value === 1
    ? 'One group is in flight'
    : `${titled(spell(flight.value))} groups are in flight`
  const doors = inside.value === 1 ? 'one is through a door' : `${spell(inside.value)} are through a door`
  const money = rate.value === null
    ? 'The fleet’s income has no baseline yet.'
    : `The fleet ${(income.value?.copperPerHour ?? 0) < 0 ? 'bleeds' : 'nets'} ${rate.value.replace('/h', '')} an hour.`
  return `${titled(spell(awake.value))} characters are awake — ${people}. ` +
    `${groups}; ${doors}. ${money}`
})

const panelStyle = {
  border: `1px solid ${V.line}`, background: V.panel, boxShadow: V.inset,
}
const capStyle = {
  fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
  letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase' as const,
  whiteSpace: 'nowrap' as const,
}
const headerStyle = {
  display: 'flex', alignItems: 'baseline', justifyContent: 'space-between',
  gap: '10px', padding: '10px 12px 6px',
}
const noteStyle = { fontFamily: FONT.mono, fontSize: '10px', color: V.faint }
const digitStyle = {
  fontFamily: FONT.mono, fontSize: '25px', color: V.textHi, lineHeight: 1.1,
  fontVariantNumeric: 'tabular-nums',
}
const digitCapStyle = {
  fontFamily: FONT.display, fontSize: '9px', fontWeight: 600,
  letterSpacing: '.16em', color: V.dim, marginTop: '4px',
}
const digitSubStyle = {
  fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint, marginTop: '3px',
}
const cellStyle = {
  appearance: 'none' as const, background: 'none', border: 'none',
  textAlign: 'left' as const, fontFamily: FONT.body, flex: 1,
  padding: '13px 18px', cursor: 'pointer',
}
</script>

<template>
  <section :style="{ padding: '18px 22px', display: 'flex', flexDirection: 'column', gap: '16px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${V.accentDim}`, paddingLeft: '15px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '17.5px',
        lineHeight: 1.4, color: V.textMid, maxWidth: '72ch', textWrap: 'pretty',
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'flex', alignItems: 'stretch', flex: 'none', ...panelStyle }">
      <button class="ov-cell" :style="cellStyle" @click="emit('goto', 'world')">
        <div :style="digitStyle">{{ fmt.int(awake) }}</div>
        <div :style="digitCapStyle">AWAKE</div>
        <div :style="digitSubStyle">across {{ fmt.int(zoneCount) }} zones &middot; map &rarr;</div>
      </button>
      <div :style="{ width: '1px', background: V.lineSoft }" />
      <button class="ov-cell" :style="cellStyle" @click="emit('goto', 'groups')">
        <div :style="digitStyle">{{ fmt.int(flight) }}</div>
        <div :style="digitCapStyle">GROUPS IN FLIGHT</div>
        <div :style="{ ...digitSubStyle, color: T.green }">{{ fmt.int(inside) }} through a door &middot; pipeline &rarr;</div>
      </button>
      <div :style="{ width: '1px', background: V.lineSoft }" />
      <button class="ov-cell" :style="cellStyle" @click="emit('goto', 'econ')">
        <div :style="{ ...digitStyle, color: rate === null ? V.faint : rateColor }">{{ rate ?? '—' }}</div>
        <div :style="digitCapStyle">FLEET INCOME</div>
        <div :style="digitSubStyle">
          {{ income?.windowMinutes ? `over the last ${income.windowMinutes}m` : 'no baseline yet' }} &middot; flow &rarr;
        </div>
      </button>
      <div :style="{ width: '1px', background: V.lineSoft }" />
      <button class="ov-cell" :style="cellStyle" @click="emit('goto', 'econ')">
        <div :style="digitStyle">{{ strip ? fmt.int(strip.listings) : '—' }}</div>
        <div :style="digitCapStyle">AH LISTINGS</div>
        <div :style="digitSubStyle">{{ strip ? `${fmt.int(strip.sellers)} sellers` : 'house unreadable' }}</div>
      </button>
    </div>

    <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '16px', alignItems: 'start' }">
      <section :style="panelStyle">
        <header :style="headerStyle">
          <span :style="capStyle">POPULATION BY ACTIVITY</span>
          <span :style="noteStyle">of {{ fmt.int(awake) }}</span>
        </header>
        <div :style="{ padding: '0 12px 12px' }">
          <p v-if="!popRows.length" :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }">
            Nobody to count yet.
          </p>
          <div
            v-for="p in popRows"
            :key="p.k"
            :style="{ display: 'grid', gridTemplateColumns: '122px 1fr 46px', gap: '10px', alignItems: 'center', padding: '2.5px 0' }"
          >
            <span :style="{ display: 'flex', alignItems: 'center', gap: '7px', fontSize: '13px', color: V.body, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', minWidth: 0 }">
              <span :style="{ width: '7px', height: '7px', flex: 'none', borderRadius: p.k === 'working' ? '0' : '50%', background: p.color, transform: p.k === 'working' ? 'rotate(45deg)' : 'none' }" />
              {{ p.label }}
            </span>
            <span :style="{ height: '6px', background: V.track, position: 'relative' }">
              <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: p.w, background: p.color, opacity: 0.9 }" />
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.text, textAlign: 'right', fontVariantNumeric: 'tabular-nums' }">{{ fmt.int(p.n) }}</span>
          </div>
        </div>
      </section>

      <section :style="panelStyle">
        <header :style="headerStyle">
          <span :style="capStyle">GROUPS IN FLIGHT</span>
          <button
            class="ov-link"
            :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontFamily: FONT.mono, fontSize: '10px', color: V.accent }"
            @click="emit('goto', 'groups')"
          >{{ conversion !== null ? `${conversion}% reach a door · ` : '' }}PVE &rarr;</button>
        </header>
        <div :style="{ padding: '0 0 6px' }">
          <p v-if="!groupRows.length" :style="{ margin: 0, padding: '0 12px 6px', fontSize: '13px', color: V.muted, lineHeight: 1.5 }">
            No parties on the road right now.
          </p>
          <button
            v-for="g in groupRows"
            :key="g.id"
            class="ov-row"
            :style="{ appearance: 'none', background: 'none', border: 'none', fontFamily: FONT.body, width: '100%', textAlign: 'left', display: 'grid', gridTemplateColumns: '60px 40px 1fr 96px', gap: '10px', padding: '6px 12px', alignItems: 'center', borderBottom: `1px solid ${V.lineFaint}`, cursor: 'pointer' }"
            @click="emit('select', g.leader)"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: g.idColor }">{{ g.id }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.faint }">{{ g.size }}</span>
            <span :style="{ fontSize: '13.5px', color: V.textHi, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ g.dest }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', letterSpacing: '.08em', color: g.stColor }">{{ g.status }}</span>
          </button>
        </div>
      </section>

      <section :style="panelStyle">
        <header :style="headerStyle">
          <span :style="capStyle">ECONOMY PULSE</span>
          <span :style="noteStyle">{{ supplyNote }}</span>
        </header>
        <div :style="{ padding: '0 12px 12px', display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '18px' }">
          <div>
            <div :style="{ fontFamily: FONT.mono, fontSize: '21px', color: rate === null ? V.faint : rateColor, fontVariantNumeric: 'tabular-nums' }">{{ rate ?? 'no rate yet' }}</div>
            <svg viewBox="0 0 100 30" preserveAspectRatio="none" :style="{ display: 'block', width: '100%', height: '34px', marginTop: '7px' }">
              <polyline v-if="sparkPoints" :points="sparkPoints" fill="none" :style="{ stroke: V.moneyDim }" stroke-width="1.5" vector-effect="non-scaling-stroke" />
            </svg>
            <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint }">
              <span>24h ago</span><span>now</span>
            </div>
          </div>
          <div>
            <div :style="{ display: 'flex', alignItems: 'flex-end', gap: '2px', height: '52px' }">
              <span
                v-for="b in bandBars"
                :key="b.band"
                :style="{ flex: 1, height: b.h, background: V.moneyDim }"
              />
            </div>
            <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint }">
              <span>level 1</span><span>gold by band</span><span>80</span>
            </div>
          </div>
        </div>
        <div :style="{ display: 'flex', gap: '14px', flexWrap: 'wrap', padding: '0 12px 12px', fontFamily: FONT.mono, fontSize: '10.5px', color: V.muted }">
          <span v-for="c in flowChips" :key="c.text" :style="{ color: c.color }">{{ c.text }}</span>
        </div>
      </section>

      <section :style="panelStyle">
        <header :style="headerStyle">
          <span :style="capStyle">ERRAND CENSUS</span>
          <span :style="noteStyle">verdicts this pass</span>
        </header>
        <div :style="{ padding: '0 12px 12px' }">
          <p v-if="!errandRows.length" :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }">
            No errand verdicts standing right now.
          </p>
          <div
            v-for="e in errandRows"
            :key="e.label"
            :style="{ display: 'grid', gridTemplateColumns: '104px 1fr 42px', gap: '10px', alignItems: 'center', padding: '3px 0' }"
          >
            <span :style="{ fontSize: '13px', color: V.body }">{{ e.label }}</span>
            <span :style="{ height: '7px', background: V.track, position: 'relative' }">
              <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: e.w, background: V.accentBar }" />
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.text, textAlign: 'right', fontVariantNumeric: 'tabular-nums' }">{{ fmt.int(e.n) }}</span>
          </div>
        </div>
      </section>

      <section :style="{ gridColumn: '1 / -1', ...panelStyle }">
        <header :style="headerStyle">
          <span :style="capStyle">EVERY CHARACTER, AT ONCE</span>
          <span :style="noteStyle">one cell = ten characters &middot; colour is what they chose to do</span>
        </header>
        <div :style="{ padding: '0 12px 12px' }">
          <p v-if="!popRows.length" :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }">
            The grid fills as characters wake.
          </p>
          <UiMatrix v-else :groups="popRows.map(p => ({ label: p.label, color: p.color, count: p.n }))" :per="10" :columns="50" />
        </div>
      </section>
    </div>
  </section>
</template>

<style scoped>
.ov-cell:hover, .ov-row:hover { background: var(--rsd); }
.ov-link:hover { color: var(--tHi); }
</style>
