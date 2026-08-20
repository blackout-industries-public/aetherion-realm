<script setup lang="ts">
import { MAPS, CLASSES, CLASS_COLOR, zoneName } from './data'

type Entity = {
  guid: number; name: string; level: number; map: number; zone: number
  x: number; y: number; cls: number; race: number; faction: string; bot: boolean
}

const { data, refresh, status } = await useFetch<{
  at: number; realmUp: boolean; stale: boolean; entities: Entity[]
}>('/api/world')

type Member = { guid: number; name: string; level: number; cls: number; map: number
  zone: number; x: number; y: number; online: boolean; instance: number
  bot: boolean; leader: boolean }
type Group = { id: number; raid: boolean; viaLfg: boolean; size: number; online: number
  members: Member[]; inInstance: boolean; allBots: boolean; maps: number[]
  minLevel: number; maxLevel: number }

const { data: groupData, refresh: refreshGroups } = await useFetch<{
  total: number; inInstances: number; raids: number; groups: Group[]
}>('/api/groups')

const selectedGroup = ref<number | null>(null)

const selectedMap = ref(571)
const faction = ref<'all' | 'alliance' | 'horde'>('all')
const minLevel = ref(1)
const showBots = ref(true)
const showHumans = ref(true)
const hovered = ref<Entity | null>(null)
const artOpacity = ref(0.75)
const hasArt = ref(false)
// Reset when switching continents so the hint reflects the map actually shown.
watch(selectedMap, () => { hasArt.value = false })

let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => {
  timer = setInterval(() => { now.value = Date.now(); refresh(); refreshGroups() }, 5000)
})
onUnmounted(() => clearInterval(timer))

const all = computed(() => data.value?.entities ?? [])

const visible = computed(() => all.value.filter(e =>
  e.map === selectedMap.value &&
  e.level >= minLevel.value &&
  (faction.value === 'all' || e.faction === faction.value) &&
  (e.bot ? showBots.value : showHumans.value)))

// The client's own projection: normalised X from world Y against LocLeft/LocRight,
// normalised Y from world X against LocTop/LocBottom. Using the real DBC bounds is
// what makes dots line up with the actual map art.
function project(e: Entity) {
  const m = MAPS[e.map]
  if (!m) return { cx: 0, cy: 0 }
  return {
    cx: ((m.left - e.y) / (m.left - m.right)) * 1000,
    cy: ((m.top - e.x) / (m.top - m.bottom)) * 667,
  }
}

const mapCounts = computed(() => {
  const counts: Record<number, number> = {}
  for (const e of all.value) counts[e.map] = (counts[e.map] ?? 0) + 1
  return counts
})

function tally<T extends string | number>(key: (e: Entity) => T, limit = 8) {
  const counts = new Map<T, number>()
  for (const e of visible.value) counts.set(key(e), (counts.get(key(e)) ?? 0) + 1)
  return [...counts.entries()].sort((a, b) => b[1] - a[1]).slice(0, limit)
}

const topZones = computed(() => tally(e => e.zone, 10))
const byClass = computed(() => tally(e => e.cls, 12))
const brackets = computed(() => {
  const counts = new Map<number, number>()
  for (const e of visible.value) {
    const b = Math.floor((e.level - 1) / 10) * 10 + 1
    counts.set(b, (counts.get(b) ?? 0) + 1)
  }
  return [...counts.entries()].sort((a, b) => a[0] - b[0])
})

const humans = computed(() => all.value.filter(e => !e.bot))

const now = ref(Date.now())
const ageLabel = computed(() => {
  const secs = Math.max(0, Math.round((now.value - (data.value?.at ?? now.value)) / 1000))
  return secs < 90 ? `${secs}s ago` : `${Math.round(secs / 60)}m ago`
})
const maxBracket = computed(() => Math.max(1, ...brackets.value.map(b => b[1])))

// Members of the highlighted group that are on the map currently shown, so the
// connector lines never jump between continents.
const activeGroup = computed(() =>
  groupData.value?.groups.find(g => g.id === selectedGroup.value) ?? null)

const groupLines = computed(() => {
  const g = activeGroup.value
  if (!g) return []
  const here = g.members.filter(m => m.online && m.map === selectedMap.value)
  const pts = here.map(m => ({ m, ...project(m as unknown as Entity) }))
  // Star from the leader (or the first member) rather than a chain, so the shape
  // reads as one group instead of a route.
  const hub = pts.find(p => p.m.leader) ?? pts[0]
  return hub ? pts.filter(p => p !== hub).map(p => ({ x1: hub.cx, y1: hub.cy, x2: p.cx, y2: p.cy })) : []
})
</script>

<template>
  <div class="app">
    <header>
      <h1>Aetherion <span class="dim">realm map</span></h1>
      <div class="totals">
        <span><b>{{ all.length }}</b> in world</span>
        <span><b>{{ humans.length }}</b> human</span>
        <span><b>{{ visible.length }}</b> shown</span>
        <span v-if="data?.stale" class="warn">
          realm restarting — last known positions, {{ ageLabel }}
        </span>
        <span v-else-if="data && !data.realmUp" class="warn">worldserver unreachable</span>
        <span v-else class="dim">{{ status === 'pending' ? 'refreshing' : 'live · 5s' }}</span>
      </div>
    </header>

    <main>
      <section class="mapwrap">
        <div class="tabs">
          <button v-for="(m, id) in MAPS" :key="id" :class="{ on: selectedMap === Number(id) }"
                  @click="selectedMap = Number(id)">
            {{ m.name }} <span class="dim">{{ mapCounts[Number(id)] ?? 0 }}</span>
          </button>
        </div>

        <svg viewBox="0 0 1000 667" class="map" :class="{ stale: data?.stale }">
          <defs>
            <pattern id="grid" width="100" height="100" patternUnits="userSpaceOnUse">
              <path d="M100 0 L0 0 0 100" fill="none" stroke="#1b2430" stroke-width="1" />
            </pattern>
          </defs>
          <rect width="1000" height="667" fill="#0d1218" />
          <!-- Grid sits under the art on purpose: with no image present the panel
               would otherwise be an empty black square. -->
          <rect width="1000" height="667" fill="url(#grid)" />
          <!-- Drop <mapId>.jpg into the mounted maps/ folder for real continent art.
               A missing file simply does not render. -->
          <!-- Both the art and the viewBox are 3:2, so no stretching is needed. -->
          <image :href="`/maps/${selectedMap}.jpg`" x="0" y="0" width="1000" height="667"
                 :opacity="artOpacity" @load="hasArt = true" />

          <g v-if="groupLines.length" stroke="#5aa9ff" stroke-width="1.5" opacity="0.8">
            <line v-for="(l, i) in groupLines" :key="i" :x1="l.x1" :y1="l.y1" :x2="l.x2" :y2="l.y2" />
          </g>

          <g>
            <circle v-for="e in visible" :key="e.guid"
                    :cx="project(e).cx" :cy="project(e).cy"
                    :r="e.bot ? 3.5 : 9"
                    :fill="CLASS_COLOR[e.cls] ?? '#8899aa'"
                    :stroke="e.bot ? 'none' : '#fff'" :stroke-width="e.bot ? 0 : 2"
                    :opacity="e.bot ? 0.8 : 1"
                    @mouseenter="hovered = e" @mouseleave="hovered = null" />
          </g>
        </svg>

        <div v-if="hovered" class="tip">
          <b>{{ hovered.name }}</b> · level {{ hovered.level }}
          {{ CLASSES[hovered.cls] ?? '?' }}
          <span class="dim">· {{ zoneName(hovered.zone) }}</span>
          <span v-if="!hovered.bot" class="human">HUMAN</span>
        </div>
        <p v-else class="tip dim">
          Hover a dot. Large ringed dots are human players.
          <template v-if="!hasArt"> · no art for this continent yet — see <code>frontend/maps/README.md</code></template>
        </p>
      </section>

      <aside>
        <div class="panel">
          <h2>Groups
            <span class="dim">{{ groupData?.total ?? 0 }} · {{ groupData?.raids ?? 0 }} raid ·
              {{ groupData?.inInstances ?? 0 }} in instance</span>
          </h2>
          <p v-if="!groupData?.groups.length" class="dim small">
            No groups yet. Bots form parties when they meet a compatible bot nearby.
          </p>
          <div v-for="g in groupData?.groups.slice(0, 12)" :key="g.id"
               class="grp" :class="{ on: selectedGroup === g.id }"
               @click="selectedGroup = selectedGroup === g.id ? null : g.id"
               @mouseenter="selectedGroup = g.id">
            <div class="grp-head">
              <b>{{ g.raid ? 'Raid' : 'Party' }} #{{ g.id }}</b>
              <span class="dim">{{ g.online }}/{{ g.size }} · lvl {{ g.minLevel }}–{{ g.maxLevel }}</span>
              <span v-if="g.inInstance" class="inst">INSTANCE</span>
              <span v-if="g.viaLfg" class="lfg">LFG</span>
              <span v-if="!g.allBots" class="human">YOU</span>
            </div>
            <div class="grp-members">
              <span v-for="m in g.members" :key="m.guid"
                    :style="{ color: CLASS_COLOR[m.cls] ?? '#8899aa', opacity: m.online ? 1 : 0.35 }">
                {{ m.leader ? '★' : '' }}{{ m.name }}<span class="dim">{{ m.level }}</span>
              </span>
            </div>
          </div>
        </div>

        <div class="panel">
          <h2>Filters</h2>
          <label>Faction
            <select v-model="faction">
              <option value="all">All</option><option value="alliance">Alliance</option>
              <option value="horde">Horde</option>
            </select>
          </label>
          <label>Min level <b>{{ minLevel }}</b>
            <input v-model.number="minLevel" type="range" min="1" max="80" />
          </label>
          <label class="check"><input v-model="showBots" type="checkbox" /> Bots</label>
          <label class="check"><input v-model="showHumans" type="checkbox" /> Humans</label>
          <label>Map art <b>{{ Math.round(artOpacity * 100) }}%</b>
            <input v-model.number="artOpacity" type="range" min="0" max="1" step="0.05" />
          </label>
        </div>

        <div class="panel">
          <h2>Level spread</h2>
          <div v-for="[b, n] in brackets" :key="b" class="bar">
            <span class="lbl">{{ b }}–{{ b + 9 }}</span>
            <span class="track"><span class="fill" :style="{ width: (n / maxBracket * 100) + '%' }" /></span>
            <span class="num">{{ n }}</span>
          </div>
        </div>

        <div class="panel">
          <h2>Busiest zones</h2>
          <div v-for="[z, n] in topZones" :key="z" class="row">
            <span>{{ zoneName(z) }}</span><b>{{ n }}</b>
          </div>
        </div>

        <div class="panel">
          <h2>Classes</h2>
          <div v-for="[c, n] in byClass" :key="c" class="row">
            <span><i class="swatch" :style="{ background: CLASS_COLOR[c] }" />{{ CLASSES[c] ?? c }}</span>
            <b>{{ n }}</b>
          </div>
        </div>
      </aside>
    </main>
  </div>
</template>

<style>
* { box-sizing: border-box; }
body { margin: 0; background: #070a0e; color: #dbe4ee;
  font: 14px/1.5 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif; }
.app { max-width: 1500px; margin: 0 auto; padding: 18px; }
header { display: flex; align-items: baseline; gap: 20px; flex-wrap: wrap; margin-bottom: 14px; }
h1 { font-size: 19px; margin: 0; font-weight: 650; letter-spacing: .2px; }
h2 { font-size: 12px; margin: 0 0 10px; text-transform: uppercase; letter-spacing: .09em; color: #8ea3ba; }
.dim { color: #7d8b9c; font-weight: 400; }
.totals { display: flex; gap: 16px; font-size: 13px; }
main { display: grid; grid-template-columns: minmax(0, 1fr) 290px; gap: 18px; align-items: start; }
@media (max-width: 980px) { main { grid-template-columns: 1fr; } }
.tabs { display: flex; gap: 6px; margin-bottom: 10px; flex-wrap: wrap; }
.tabs button { background: #121a24; color: #b9c8d8; border: 1px solid #1e2a38;
  border-radius: 7px; padding: 6px 11px; cursor: pointer; font-size: 13px; }
.tabs button.on { background: #1d3350; border-color: #2f5788; color: #fff; }
.map { width: 100%; aspect-ratio: 1000 / 667; border: 1px solid #1b2430; border-radius: 10px; display: block; }
.map circle { transition: r .1s ease; cursor: crosshair; }
.tip { margin: 9px 2px 0; font-size: 13px; min-height: 20px; }
.small { font-size: 12px; }
.grp { border: 1px solid #1b2430; border-radius: 7px; padding: 7px 9px; margin-bottom: 7px; cursor: pointer; }
.grp.on { border-color: #2f5788; background: #101c2c; }
.grp-head { display: flex; gap: 8px; align-items: baseline; flex-wrap: wrap; font-size: 12px; }
.grp-members { display: flex; gap: 8px; flex-wrap: wrap; margin-top: 4px; font-size: 11px; }
.grp-members .dim { margin-left: 2px; }
.lfg { background: #2a2340; color: #b9a8ee; border-radius: 4px; padding: 0 5px; font-size: 10px; }
.inst { background: #14432a; color: #7ee2a8; border-radius: 4px; padding: 0 5px; font-size: 10px; }
.warn { background: #4a2c12; color: #ffcf8b; border-radius: 5px; padding: 2px 9px; font-size: 12px; }
.map.stale { opacity: .55; filter: saturate(.5); }
.human { background: #2f5788; color: #fff; border-radius: 4px; padding: 1px 6px; margin-left: 8px; font-size: 11px; }
.panel { background: #0d141c; border: 1px solid #1b2430; border-radius: 10px; padding: 13px; margin-bottom: 13px; }
label { display: block; font-size: 13px; margin-bottom: 11px; color: #b9c8d8; }
label.check { display: flex; align-items: center; gap: 7px; }
select, input[type=range] { width: 100%; margin-top: 5px; }
select { background: #121a24; color: #dbe4ee; border: 1px solid #1e2a38; border-radius: 6px; padding: 5px; }
.row { display: flex; justify-content: space-between; gap: 10px; padding: 2px 0; font-size: 13px; }
.swatch { display: inline-block; width: 9px; height: 9px; border-radius: 2px; margin-right: 7px; }
.bar { display: grid; grid-template-columns: 54px 1fr 32px; gap: 8px; align-items: center; font-size: 12px; margin-bottom: 4px; }
.track { background: #121a24; border-radius: 3px; height: 9px; overflow: hidden; }
.fill { display: block; height: 100%; background: #2f5788; }
.num { text-align: right; color: #8ea3ba; }
</style>
