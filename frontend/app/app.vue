<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { T, FONT, fmt } from './theme'
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
})

const TABS = [
  { key: 'world', label: 'WORLD' },
  { key: 'groups', label: 'PVE' },
  { key: 'pvp', label: 'PVP' },
  { key: 'race', label: 'RACE' },
  { key: 'econ', label: 'ECON' },
  { key: 'society', label: 'SOCIETY' },
  { key: 'guilds', label: 'GUILDS' },
  { key: 'ops', label: 'OPS' },
] as const
type TabKey = (typeof TABS)[number]['key']
const tab = ref<TabKey>('world')

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
const humans = computed(() => entities.value.filter(e => !e.bot).length)

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

let fast: ReturnType<typeof setInterval> | undefined
let slow: ReturnType<typeof setInterval> | undefined
let tick: ReturnType<typeof setInterval> | undefined

onMounted(() => {
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
onUnmounted(() => { clearInterval(fast); clearInterval(slow); clearInterval(tick) })
</script>

<template>
  <div
    :style="{
      height: '100vh', display: 'grid', gridTemplateRows: '54px 1fr',
      background: T.bg, color: T.textHi, fontFamily: FONT.body, overflow: 'hidden',
    }"
  >
    <header
      :style="{
        display: 'flex', alignItems: 'stretch',
        borderBottom: '1px solid oklch(0.45 0.06 82 / .45)',
        background: `linear-gradient(${T.raised}, oklch(0.16 0.017 50))`,
        overflow: 'hidden', whiteSpace: 'nowrap',
      }"
    >
      <div :style="{ display: 'flex', alignItems: 'center', gap: '10px', padding: '0 18px', borderRight: `1px solid ${T.line}`, minWidth: 0 }">
        <span
          :style="{
            width: '7px', height: '7px', background: world?.realmUp ? T.gold : T.red,
            borderRadius: '50%', animation: 'blip 2.4s ease-in-out infinite', flex: 'none',
          }"
        />
        <span
          :style="{
            fontFamily: FONT.display, fontSize: '18px', fontWeight: 700,
            letterSpacing: '.15em', textTransform: 'uppercase', color: T.goldBright,
          }"
        >Aetherion</span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', letterSpacing: '.10em', color: T.dim, paddingTop: '2px' }">
          {{ ops?.address ?? '' }}
        </span>
      </div>

      <nav :style="{ display: 'flex', alignItems: 'stretch', flex: 'none' }">
        <button
          v-for="t in TABS"
          :key="t.key"
          :style="{
            appearance: 'none', background: 'none', border: 'none',
            borderBottom: `2px solid ${tab === t.key ? T.gold : 'transparent'}`,
            color: tab === t.key ? T.goldBright : T.dim,
            fontFamily: FONT.display, fontSize: '12px', fontWeight: 600,
            letterSpacing: '.16em', padding: '0 17px', cursor: 'pointer',
          }"
          @click="tab = t.key"
        >{{ t.label }}</button>
      </nav>

      <div :style="{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: '22px', padding: '0 18px', minWidth: 0 }">
        <span v-for="s in [
          { v: fmt.int(entities.length), l: 'characters' },
          { v: fmt.int(humans), l: 'humans' },
        ]" :key="s.l" :style="{ textAlign: 'right' }">
          <span :style="{ fontFamily: FONT.mono, fontSize: '17px', color: T.textHi, display: 'block', lineHeight: 1.1 }">{{ s.v }}</span>
          <span :style="{ fontFamily: FONT.display, fontSize: '9px', letterSpacing: '.14em', color: T.dim, textTransform: 'uppercase' }">{{ s.l }}</span>
        </span>

        <span :style="{ display: 'flex', alignItems: 'center', gap: '7px' }">
          <span :style="{ width: '5px', height: '5px', borderRadius: '50%', background: world?.stale ? T.red : T.green }" />
          <span :style="{ fontFamily: FONT.display, fontSize: '9.5px', letterSpacing: '.14em', color: world?.stale ? T.red : T.dim }">
            {{ world?.stale ? 'STALE' : 'LIVE' }}
          </span>
        </span>
      </div>
    </header>

    <div :style="{ display: 'grid', gridTemplateColumns: 'minmax(0, 1fr) clamp(280px, 24vw, 380px)', minHeight: 0, minWidth: 0, overflow: 'hidden' }">
      <WorldView
        v-if="tab === 'world'"
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
      <EconView v-else-if="tab === 'econ'" :econ="econ" @select="select" />
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
      />
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
button:focus-visible { outline: 2px solid oklch(0.80 0.10 88); outline-offset: -2px }
@keyframes blip { 0%, 100% { opacity: 1 } 50% { opacity: .3 } }
@media (prefers-reduced-motion: reduce) { * { animation: none !important } }
</style>
