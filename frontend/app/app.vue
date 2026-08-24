<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import {
  T, V, FONT, STATE, fmt, PALETTES, PALETTE_CSS, THEME_CHOICES,
  type PaletteName, type StateKey,
} from './theme'
import { CLASS_COLOR, zoneName } from './data'
import WorldView from './components/WorldView.vue'
import GroupsView from './components/GroupsView.vue'
import SocietyView from './components/SocietyView.vue'
import OpsView from './components/OpsView.vue'
import GuildsView from './components/GuildsView.vue'
import PvpView from './components/PvpView.vue'
import RaceView from './components/RaceView.vue'
import EventRail from './components/EventRail.vue'

type Entity = {
  guid: number; name: string; level: number; map: number; zone: number
  x: number; y: number; cls: number; race: number; faction: string; bot: boolean
  dead: boolean; ghost: boolean; instance: number; grouped: boolean
  rpg?: number; trade?: string; travelling?: boolean; place?: string | null
}

useHead({
  title: 'Aetherion Observatory',
  link: [
    { rel: 'preconnect', href: 'https://fonts.googleapis.com' },
    { rel: 'preconnect', href: 'https://fonts.gstatic.com', crossorigin: '' },
    {
      rel: 'stylesheet',
      href: 'https://fonts.googleapis.com/css2?family=Cinzel:wght@400;600;700&family=Cutive+Mono&family=Spectral:ital,wght@0,300;0,400;0,600;1,300;1,400&display=swap',
    },
  ],
  style: [{ textContent: PALETTE_CSS }],
})

type TabKey = 'overview' | 'world' | 'groups' | 'pvp' | 'race' | 'econ' | 'market'
  | 'society' | 'guilds' | 'ops'
const tab = ref<TabKey>('overview')

// Grouped navigation. OVERVIEW joins the LIVE group as one more entry when the
// tab lands.
const NAV_GROUPS: { label: string; tabs: { key: TabKey; label: string }[] }[] = [
  {
    label: 'LIVE',
    tabs: [
      { key: 'overview', label: 'OVERVIEW' },
      { key: 'world', label: 'WORLD' },
      { key: 'groups', label: 'PVE' },
      { key: 'pvp', label: 'PVP' },
      { key: 'race', label: 'RACE' },
    ],
  },
  {
    label: 'ECONOMY',
    tabs: [{ key: 'econ', label: 'ECON' }, { key: 'market', label: 'MARKET' }],
  },
  {
    label: 'SOCIAL',
    tabs: [{ key: 'society', label: 'SOCIETY' }, { key: 'guilds', label: 'GUILDS' }],
  },
  { label: 'SYSTEM', tabs: [{ key: 'ops', label: 'OPS' }] },
]

const POLL_MS = 5000

const { data: world, refresh: refreshWorld } = await useFetch<{
  at: number; realmUp: boolean; stale: boolean; entities: Entity[]
}>('/api/world')

const { data: assembler, refresh: refreshAssembler } = await useFetch<any>('/api/assembler')
const { data: events, refresh: refreshEvents } = await useFetch<any>('/api/events')
const { data: guilds, refresh: refreshGuilds } = await useFetch<any>('/api/guilds')
const { data: combat, refresh: refreshCombat } = await useFetch<any>('/api/combat')
const { data: llm, refresh: refreshLlm } = await useFetch<any>('/api/llm')
const { data: ops, refresh: refreshOps } = await useFetch<any>('/api/ops')
const { data: society, refresh: refreshSociety } = await useFetch<any>('/api/society')
const { data: guild, refresh: refreshGuild } = await useFetch<any>('/api/guild')
const { data: pvp, refresh: refreshPvp } = await useFetch<any>('/api/pvp')
const { data: quests, refresh: refreshQuests } = await useFetch<any>('/api/quests')
const { data: race, refresh: refreshRace } = await useFetch<any>('/api/race')
const { data: econ, refresh: refreshEcon } = await useFetch<any>('/api/econ')

const entities = computed<Entity[]>(() => world.value?.entities ?? [])

// Movement is inferred by comparing consecutive polls: the schema has no "is moving"
// flag, and positions only save periodically, so this is indicative rather than exact.
const lastSeen = new Map<number, string>()
const moving = ref<Set<number>>(new Set())

// Nothing server-side records where a character has been, so trails are accumulated
// from polls while the page is open. A freshly loaded page has no history yet.
type Point = { map: number; x: number; y: number }
const TRAIL_MAX = 120
const trails = new Map<number, Point[]>()
const trailTick = ref(0)

function refreshMovement(list: Entity[]) {
  const next = new Set<number>()
  for (const e of list) {
    const key = `${e.map}:${e.x}:${e.y}`
    const changed = lastSeen.has(e.guid) && lastSeen.get(e.guid) !== key
    if (changed) next.add(e.guid)
    lastSeen.set(e.guid, key)

    if (changed || !trails.has(e.guid)) {
      const path = trails.get(e.guid) ?? []
      path.push({ map: e.map, x: e.x, y: e.y })
      if (path.length > TRAIL_MAX) path.shift()
      trails.set(e.guid, path)
    }
  }
  moving.value = next
  trailTick.value++
}
watch(entities, list => refreshMovement(list), { immediate: true })

// Selection is by name because every panel has a name to hand; the rail resolves it to
// history and personality.
const selected = ref<string | null>(null)
const detail = ref<any | null>(null)
const loadingDetail = ref(false)

async function select(name: string) {
  if (!name) return
  selected.value = name
  detail.value = null
  loadingDetail.value = true
  try {
    // One call: the bridge's history response already carries personality and level.
    const history = await $fetch<any>(`/api/bot/${name}/history`).catch(() => null)
    const ent = entities.value.find(e => e.name === name)
    detail.value = { ...(history ?? {}), cls: ent?.cls ?? 0, level: history?.level ?? ent?.level ?? 0 }
  } finally {
    loadingDetail.value = false
  }
}

const selectedGuid = computed(() =>
  entities.value.find(e => e.name === selected.value)?.guid ?? null)

// What this character is doing right now, in one sentence. The event feed says what
// happened; this says what is happening, which is the question people actually ask of
// a bot they just clicked on.
const activity = computed(() => {
  const name = selected.value
  if (!name) return null

  const row = (assembler.value?.board ?? []).find((b: any) =>
    b.leader === name || b.members?.some((m: any) => m.name === name))

  if (row) {
    const mine = row.members?.find((m: any) => m.name === name)
    const role = mine ? `${mine.role} in ` : ''
    const party = `${role}${row.label}`
    if (row.tone === 'inside') return `Inside ${row.dest}, ${party}.`
    if (row.tone === 'door') return `At the door of ${row.dest}, waiting on the summon, ${party}.`
    const how = row.viaPlace
      ? ` They ${row.via.toLowerCase()} to ${row.viaPlace}${row.viaActor ? ` on ${row.viaActor}'s portal` : ''}.`
      : ''
    return `Travelling to ${row.dest}, ${row.remaining} out, ${party}.${how}`
  }

  const e = entities.value.find(x => x.name === name)
  if (!e) return null
  if (e.dead) return 'Dead.'
  if (e.ghost) return 'Running back to their corpse.'
  if (e.instance > 0) {
    const where = e.place ? ` in ${e.place}` : ' in an instance'
    // No trip means the assembler is not steering them - worth saying, because that is
    // usually how a character ends up sitting somewhere for hours.
    return `Inside${where}, not on an assembler run${e.grouped ? '' : ' and not in a group'}.`
  }
  if (e.grouped) return 'In a group, with nowhere booked.'
  return 'Out in the world on their own.'
})

const now = ref(Date.now())
const refreshAgo = computed(() => Math.max(0, (now.value - (world.value?.at ?? now.value)) / 1000))

// Position writes are what actually gate the map, not the poll interval.
const saveInterval = computed(() => 60)

// --- palette ------------------------------------------------------------------

const theme = ref<PaletteName>('gold')

function setTheme(name: PaletteName) {
  theme.value = name
  try { localStorage.setItem('aeth-theme', name) } catch { /* private mode */ }
}

// --- alerts -------------------------------------------------------------------

type Alert = { key: string; tone: string; text: string; tab: TabKey; jump: string }

const alerts = computed<Alert[]>(() => {
  const list: Alert[] = []
  const err = ops.value?.dbError
  if (err?.message)
    list.push({ key: 'db', tone: T.red, text: `database: ${String(err.message).slice(0, 90)}`, tab: 'ops', jump: 'OPS' })
  for (const c of ops.value?.containers ?? [])
    if (!c.up) list.push({ key: `down-${c.name}`, tone: T.red, text: `${c.name} is not answering`, tab: 'ops', jump: 'OPS' })
  const stale = (ops.value?.freshness ?? []).filter((f: any) => f.status === 'stale')
  if (stale.length)
    list.push({
      key: 'stale', tone: V.accent, tab: 'ops', jump: 'OPS',
      text: `${stale.map((f: any) => f.feed).join(', ')} ${stale.length === 1 ? 'feed has' : 'feeds have'} gone stale`,
    })
  // Errand rows are the verdict census, not money needs - they never fund.
  // Only PRICED needs can alarm: unpriced gear wants are the realm's standing
  // condition, and an alert that never clears is just wallpaper. This matches
  // the ECON lede's definition, so strip and lede can never contradict.
  const needs = (econ.value?.needs ?? []).filter((n: any) => n.type !== 'errand')
  const unfunded = needs.reduce((s: number, n: any) => {
    const priced = n.priced ?? (n.type === 'gear' ? 0 : n.n)
    return s + Math.max(0, priced - n.funded)
  }, 0)
  if (unfunded > 0) {
    const oldest = (econ.value?.starved ?? [])[0]
    list.push({
      key: 'needs', tone: V.accent, tab: 'econ', jump: 'ECON',
      text: `${fmt.int(unfunded)} priced needs unfunded${oldest ? `, oldest ${fmt.ago(oldest.since)}` : ''}`,
    })
  }
  return list.slice(0, 3)
})

// Dismissal is keyed to the current set: a new kind of trouble re-opens the strip.
const dismissedSig = ref<string | null>(null)
const alertSig = computed(() => alerts.value.map(a => a.key).join('|'))
const alertsVisible = computed(() => alerts.value.length > 0 && dismissedSig.value !== alertSig.value)

const econAlert = computed(() => alerts.value.some(a => a.tab === 'econ'))
const opsAlert = computed(() => alerts.value.some(a => a.tab === 'ops'))

function navDot(key: TabKey) {
  if (key === 'econ' && econAlert.value) return V.accent
  if (key === 'ops' && opsAlert.value) return T.red
  return null
}

// --- character search ---------------------------------------------------------

const searchOpen = ref(false)
const query = ref('')
const searchInput = ref<HTMLInputElement | null>(null)

function openSearch() {
  searchOpen.value = true
  query.value = ''
  nextTick(() => searchInput.value?.focus())
}

// Same precedence WorldView uses for its dots, so the search sub-line and the map
// never disagree about what someone is doing.
function liteState(e: Entity): StateKey {
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
  if (e.travelling || moving.value.has(e.guid)) return 'travelling'
  if (e.grouped) return 'grouped'
  return 'idle'
}

const searchResults = computed(() => {
  const q = query.value.trim().toLowerCase()
  const hits = q ? entities.value.filter(e => e.name.toLowerCase().includes(q)) : entities.value
  return hits.slice(0, 7).map(e => ({
    name: e.name,
    color: CLASS_COLOR[e.cls] ?? V.textHi,
    level: e.level,
    sub: `${zoneName(e.zone).toLowerCase()} · ${STATE[liteState(e)].label}`,
  }))
})

function pick(name: string) {
  searchOpen.value = false
  select(name)
}

function onKeydown(ev: KeyboardEvent) {
  if ((ev.metaKey || ev.ctrlKey) && ev.key.toLowerCase() === 'k') {
    ev.preventDefault()
    openSearch()
  } else if (ev.key === 'Escape' && searchOpen.value) {
    searchOpen.value = false
  }
}

let fast: ReturnType<typeof setInterval> | undefined
let slow: ReturnType<typeof setInterval> | undefined
let tick: ReturnType<typeof setInterval> | undefined

onMounted(() => {
  // Applied after mount so the server-rendered gold shell never mismatches.
  try {
    const saved = localStorage.getItem('aeth-theme') as PaletteName | null
    if (saved && saved in PALETTES) theme.value = saved
  } catch { /* private mode */ }
  window.addEventListener('keydown', onKeydown)

  tick = setInterval(() => { now.value = Date.now() }, 1000)
  fast = setInterval(() => {
    refreshWorld(); refreshAssembler(); refreshEvents(); refreshPvp()
  }, POLL_MS)
  // Guild standings, LLM metrics and host stats move on a scale of minutes.
  slow = setInterval(() => {
    refreshGuilds(); refreshCombat(); refreshLlm(); refreshOps()
    refreshSociety(); refreshGuild(); refreshQuests(); refreshRace(); refreshEcon()
  }, 30000)
})
onUnmounted(() => {
  window.removeEventListener('keydown', onKeydown)
  clearInterval(fast); clearInterval(slow); clearInterval(tick)
})
</script>

<template>
  <div
    :data-aeth="theme"
    :style="{
      height: '100vh', display: 'grid', gridTemplateRows: '58px auto 1fr',
      background: V.bg, color: V.body, fontFamily: FONT.body, overflow: 'hidden',
    }"
  >
    <header
      :style="{
        display: 'flex', alignItems: 'stretch',
        borderBottom: `1px solid ${V.lineAccent}`,
        background: `linear-gradient(${V.raised}, ${V.panelDeep})`,
        overflow: 'hidden', whiteSpace: 'nowrap',
      }"
    >
      <div :style="{ display: 'flex', alignItems: 'center', gap: '10px', padding: '0 18px', borderRight: `1px solid ${V.line}` }">
        <span
          :style="{
            width: '7px', height: '7px', background: world?.realmUp ? V.accent : T.red,
            borderRadius: '50%', animation: 'blip 2.4s ease-in-out infinite', flex: 'none',
          }"
        />
        <span :style="{ display: 'flex', flexDirection: 'column', lineHeight: 1.1 }">
          <span :style="{ fontFamily: FONT.display, fontSize: '16px', fontWeight: 700, letterSpacing: '.15em', color: V.accentBright }">AETHERION</span>
          <span :style="{ fontFamily: FONT.display, fontSize: '7.5px', fontWeight: 600, letterSpacing: '.34em', color: V.dim }">OBSERVATORY</span>
        </span>
      </div>

      <nav :style="{ display: 'flex', alignItems: 'stretch' }">
        <div
          v-for="(g, gi) in NAV_GROUPS"
          :key="g.label"
          :style="{ display: 'flex', alignItems: 'stretch' }"
        >
          <div :style="{ display: 'flex', flexDirection: 'column', justifyContent: 'center', padding: '0 6px' }">
            <span
              :style="{
                fontFamily: FONT.display, fontSize: '7px', fontWeight: 600,
                letterSpacing: '.26em', color: V.faint2, textAlign: 'center',
                padding: '4px 0 2px',
              }"
            >{{ g.label }}</span>
            <span :style="{ display: 'flex' }">
              <button
                v-for="t in g.tabs"
                :key="t.key"
                :style="{
                  appearance: 'none', background: 'none', border: 'none',
                  borderBottom: `2px solid ${tab === t.key ? V.accent : 'transparent'}`,
                  color: tab === t.key ? V.accentBright : V.dim,
                  fontFamily: FONT.display, fontSize: '11px', fontWeight: 600,
                  letterSpacing: '.13em', padding: '2px 11px 8px', cursor: 'pointer',
                  position: 'relative',
                }"
                @click="tab = t.key"
              >{{ t.label }}<span
                v-if="navDot(t.key)"
                :style="{
                  position: 'absolute', top: 0, right: '2px', width: '5px', height: '5px',
                  borderRadius: '50%', background: navDot(t.key) || 'transparent',
                }"
              /></button>
            </span>
          </div>
          <div
            v-if="gi < NAV_GROUPS.length - 1"
            :style="{ width: '1px', background: V.lineSoft, margin: '12px 4px' }"
          />
        </div>
      </nav>

      <div :style="{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: '12px', padding: '0 14px', minWidth: 0, justifyContent: 'flex-end' }">
        <span
          :style="{ display: 'flex', alignItems: 'center', gap: '7px', border: `1px solid ${V.line}`, background: V.panel, padding: '6px 10px' }"
          title="Palette"
        >
          <button
            v-for="c in THEME_CHOICES"
            :key="c.key"
            :title="c.name"
            :style="{
              appearance: 'none', width: '13px', height: '13px', borderRadius: '50%',
              background: PALETTES[c.key].accent,
              border: `2px solid ${theme === c.key ? V.textHi : V.lineSoft}`,
              cursor: 'pointer', padding: 0,
            }"
            @click="setTheme(c.key)"
          />
        </span>

        <button
          :style="{
            appearance: 'none', display: 'flex', alignItems: 'center', gap: '9px',
            border: `1px solid ${V.line}`, background: V.panel, padding: '7px 12px',
            flex: '0 1 238px', minWidth: '118px', cursor: 'text',
          }"
          @click="openSearch"
        >
          <span :style="{ color: V.faint, fontSize: '13px' }">⌕</span>
          <span
            :style="{
              fontSize: '13px', color: V.faint, fontStyle: 'italic', flex: 1,
              textAlign: 'left', fontFamily: FONT.body, overflow: 'hidden',
              whiteSpace: 'nowrap', textOverflow: 'ellipsis', minWidth: 0,
            }"
          >Find a character…</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint, border: `1px solid ${V.lineSoft}`, padding: '1px 5px' }">⌘K</span>
        </button>

        <span :style="{ textAlign: 'right' }">
          <span :style="{ fontFamily: FONT.mono, fontSize: '16px', color: V.textHi, display: 'block', lineHeight: 1.1 }">{{ fmt.int(entities.length) }}</span>
          <span :style="{ fontFamily: FONT.display, fontSize: '8.5px', fontWeight: 600, letterSpacing: '.14em', color: V.dim }">CHARACTERS</span>
        </span>

        <span :style="{ display: 'flex', alignItems: 'center', gap: '7px' }">
          <span :style="{ width: '5px', height: '5px', borderRadius: '50%', background: world?.stale ? T.red : T.green }" />
          <span :style="{ fontFamily: FONT.display, fontSize: '9.5px', fontWeight: 600, letterSpacing: '.14em', color: world?.stale ? T.red : V.dim }">
            {{ world?.stale ? 'STALE' : 'LIVE' }}
          </span>
        </span>
      </div>
    </header>

    <div
      v-if="alertsVisible"
      :style="{
        display: 'flex', alignItems: 'center', gap: '20px', padding: '8px 22px',
        background: V.bgAlt, borderBottom: `1px solid ${V.lineSoft}`,
      }"
    >
      <span :style="{ fontFamily: FONT.display, fontSize: '8.5px', fontWeight: 600, letterSpacing: '.22em', color: V.dim }">ALERTS</span>
      <button
        v-for="a in alerts"
        :key="a.key"
        class="hv-fade"
        :style="{
          appearance: 'none', background: 'none', border: 'none', padding: 0,
          display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer',
          fontFamily: FONT.body,
        }"
        @click="tab = a.tab"
      >
        <span :style="{ width: '6px', height: '6px', background: a.tone, flex: 'none' }" />
        <span :style="{ fontSize: '13px', color: V.textMid }">{{ a.text }}</span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.accent, letterSpacing: '.06em' }">→ {{ a.jump }}</span>
      </button>
      <button
        :style="{
          appearance: 'none', background: 'none', border: 'none', marginLeft: 'auto',
          fontFamily: FONT.mono, fontSize: '10px', color: V.faint, cursor: 'pointer', padding: 0,
        }"
        @click="dismissedSig = alertSig"
      >dismiss all</button>
    </div>
    <div v-else />

    <div :style="{ display: 'grid', gridTemplateColumns: 'minmax(0, 1fr) clamp(300px, 24vw, 380px)', minHeight: 0, minWidth: 0, overflow: 'hidden' }">
      <OverviewView
        v-if="tab === 'overview'"
        @select="select"
        @goto="(k: TabKey) => (tab = k)"
      />
      <WorldView
        v-else-if="tab === 'world'"
        :entities="entities"
        :moving="moving"
        :trails="trails"
        :trail-tick="trailTick"
        :selected-guid="selectedGuid"
        :refresh-ago="refreshAgo"
        :refresh-every="saveInterval"
        :professions="world?.professions ?? []"
        @select="select"
      />
      <GroupsView v-else-if="tab === 'groups'" :assembler="assembler" :quests="quests" @select="select" />
      <SocietyView v-else-if="tab === 'society'" :guilds="guilds" :combat="combat" :llm="llm" :society="society" @select="select" />
      <PvpView v-else-if="tab === 'pvp'" :pvp="pvp" @select="select" />
      <RaceView v-else-if="tab === 'race'" :race="race" @select="select" />
      <div v-else-if="tab === 'econ'" :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <EconView :econ="econ" @select="select" />
        <PulseView @select="select" />
        <IndustryView @select="select" />
      </div>
      <div v-else-if="tab === 'market'" :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <MarketView @select="select" />
        <WealthView @select="select" />
      </div>
      <GuildsView v-else-if="tab === 'guilds'" :guild="guild" @select="select" />
      <OpsView v-else :ops="ops" :assembler="assembler" :llm="llm" />

      <EventRail
        :events="events?.events ?? []"
        :ready="!!events?.ready"
        :selected="selected"
        :detail="detail"
        :activity="activity"
        :loading="loadingDetail"
        @select="select"
        @clear="selected = null"
        @map="tab = 'world'"
      />
    </div>

    <div
      v-if="searchOpen"
      :style="{
        position: 'fixed', inset: 0, background: 'oklch(0.09 0.01 48 / .62)',
        zIndex: 50, display: 'flex', alignItems: 'flex-start', justifyContent: 'center',
        paddingTop: '110px',
      }"
      @click="searchOpen = false"
    >
      <div
        :style="{
          width: '540px', maxWidth: '92vw', border: `1px solid ${V.lineAccentSoft}`,
          background: V.panelDeep,
          boxShadow: `0 18px 60px oklch(0.05 0.008 45 / .8), ${V.inset}`,
        }"
        @click.stop
      >
        <div :style="{ display: 'flex', alignItems: 'center', gap: '10px', padding: '13px 16px', borderBottom: `1px solid ${V.line}` }">
          <span :style="{ color: V.faint, fontSize: '15px' }">⌕</span>
          <input
            ref="searchInput"
            v-model="query"
            placeholder="Type a name — Thal, Vex, Grim…"
            :style="{
              flex: 1, appearance: 'none', background: 'none', border: 'none',
              outline: 'none', fontFamily: FONT.body, fontSize: '16px', color: V.textHi,
            }"
            @keydown.enter="searchResults[0] && pick(searchResults[0].name)"
          >
          <button
            :style="{
              appearance: 'none', background: 'none', border: `1px solid ${V.lineSoft}`,
              color: V.faint, fontFamily: FONT.mono, fontSize: '9.5px',
              padding: '2px 6px', cursor: 'pointer',
            }"
            @click="searchOpen = false"
          >ESC</button>
        </div>
        <div :style="{ maxHeight: '340px', overflow: 'auto' }">
          <button
            v-for="r in searchResults"
            :key="r.name"
            class="hv-raised"
            :style="{
              appearance: 'none', border: 'none', borderBottom: `1px solid ${V.lineFaint}`,
              width: '100%', textAlign: 'left', display: 'grid',
              gridTemplateColumns: '1fr auto auto', gap: '12px', alignItems: 'baseline',
              padding: '9px 16px', cursor: 'pointer', fontFamily: FONT.body,
            }"
            @click="pick(r.name)"
          >
            <span :style="{ fontSize: '14.5px', color: r.color, fontWeight: 500 }">{{ r.name }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.muted }">lv {{ r.level }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint }">{{ r.sub }}</span>
          </button>
          <p
            v-if="query.trim() && !searchResults.length"
            :style="{ margin: 0, padding: '14px 16px', fontSize: '13px', color: V.faint }"
          >Nobody by that name is awake.</p>
        </div>
        <div :style="{ padding: '8px 16px', fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint2, borderTop: `1px solid ${V.lineFaint}` }">
          ↵ open dossier · esc close · {{ fmt.int(entities.length) }} characters indexed
        </div>
      </div>
    </div>
  </div>
</template>

<style>
* { box-sizing: border-box }
/* The shell is a fixed-viewport app, never a scrolling document. Pinning this at the
   root means no descendant can push the page sideways and clip the header. */
html, body, #__nuxt {
  margin: 0; padding: 0; height: 100%; max-width: 100%;
  overflow-x: hidden; background: oklch(0.145 0.016 52);
}
body { -webkit-font-smoothing: antialiased }
::-webkit-scrollbar { width: 9px; height: 9px }
::-webkit-scrollbar-thumb { background: oklch(0.34 0.03 62) }
::-webkit-scrollbar-track { background: transparent }
button:focus-visible { outline: 2px solid var(--acc); outline-offset: -2px }
/* Hover states live in classes because inline styles cannot express :hover. */
.hv-row { background: none }
.hv-row:hover { background: var(--pnl) }
.hv-raised { background: none }
.hv-raised:hover { background: var(--rsd) }
.hv-fade:hover { opacity: .82 }
@keyframes blip { 0%, 100% { opacity: 1 } 50% { opacity: .3 } }
@media (prefers-reduced-motion: reduce) { * { animation: none !important } }
</style>
