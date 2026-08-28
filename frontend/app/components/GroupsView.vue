<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { T, FONT, V, fmt, spell, titled } from '../theme'
import UiPanel from './UiPanel.vue'
import UiEncounters from './UiEncounters.vue'
import UiSkull from './UiSkull.vue'
import QuestPanels from './QuestPanels.vue'
import { CLASS_COLOR } from '../data'

type Encounter = { name: string; killed: boolean }

const props = defineProps<{ assembler: any | null; quests: any | null }>()
const emit = defineEmits<{ select: [string] }>()

// Stage colours: chrome for the quiet ends, fixed data colours for the two that carry
// meaning across palettes - the door pink and the semantic green.
const DOOR_PINK = 'oklch(0.82 0.08 350)'
const STAGE_COLOR: Record<string, string> = {
  forming: V.dim, travel: V.accentBright, door: DOOR_PINK, inside: T.green,
}

const RARITY_COLOR: Record<string, string> = {
  uncommon: 'oklch(0.74 0.10 158)',
  rare: 'oklch(0.70 0.13 250)',
  epic: 'oklch(0.68 0.15 305)',
}

// Card pips stop at five; the note carries the real n/m so a deep dungeon never lies.
const MAX_PIPS = 5

const board = computed(() => props.assembler?.board ?? [])

const lastWord = (s: string) => s.trim().split(/\s+/).pop() ?? s

const card = (p: any) => {
  const encounters: Encounter[] = p.encounters ?? []
  const downed = encounters.filter(b => b.killed)
  const cleared = encounters.length > 0 && downed.length === encounters.length
  const final = encounters[encounters.length - 1]
  const deaths = `${p.deaths} death${p.deaths === 1 ? '' : 's'}`
  const drops = `${p.looted} drop${p.looted === 1 ? '' : 's'}`
  const via = String(p.via).toLowerCase()
  const raidBots = p.isRaid ? `${String(p.size).split('/')[0]} bots` : null

  return {
    key: p.id,
    raid: !!p.isRaid,
    id: `${p.isRaid ? 'R' : 'G'}-${String(p.id).padStart(3, '0')}${p.isRaid ? ' · RAID' : ''}`,
    idColor: p.isRaid ? V.accent : V.muted,
    note: p.tone === 'inside' ? `${p.dwellMins ?? 0}m in` : p.size,
    noteColor: p.tone === 'inside' ? T.green : V.faint,
    dest: p.dest,
    heroic: !!p.heroic,
    sub: p.tone === 'travel' ? `${via} · ${p.remaining} out`
      : p.tone === 'door' ? `waiting on the summon · ${p.tripMins ?? 0}m under way`
      : p.isRaid ? `${via} · ${raidBots} · ${deaths}`
      : `${via} · ${deaths} · ${drops}`,
    leader: p.leader,
    ledBy: p.tone !== 'inside' && p.isRaid ? p.leader : null,
    pips: p.tone === 'inside'
      ? encounters.slice(0, MAX_PIPS).map(b => b.killed)
      : [],
    pipNote: !encounters.length ? ''
      : cleared ? 'CLEARED'
      : `${downed.length}/${encounters.length}${final && !final.killed ? ` · ${lastWord(final.name)} alive` : ''}`,
    pipNoteColor: cleared ? T.green : V.faint,
    downed: downed.length
      ? downed.slice(0, 4).map(b => b.name).join(' · ')
        + (downed.length > 4 ? ` · +${downed.length - 4} more` : '')
      : null,
    loot: p.tone === 'inside' && p.looted > 0 ? `${drops}${p.loot ? ' · ' : ''}` : null,
    lootItem: p.loot?.name ?? '',
    lootColor: RARITY_COLOR[p.loot?.rarity ?? ''] ?? V.text,
    progW: p.tone === 'travel' ? `${p.pct}%` : null,
    // The expanded run dossier's raw material.
    members: p.members ?? [],
    encounterList: encounters,
    dropsList: (p.drops ?? []).map((d: any) => ({
      name: d.name, color: RARITY_COLOR[d.rarity] ?? V.text,
    })),
  }
}

// One run open at a time, per the handoff; the card is the toggle and names
// inside it stay doors to the character dossier.
const open = ref<string | number | null>(null)

// History is self-fetched: it has its own table and cadence, and the shell's
// assembler payload should stay a live mirror rather than grow an archive.
const { data: runsData } = useFetch<any>('/api/runs', { server: false })
let runsTimer: ReturnType<typeof setInterval> | null = null
onMounted(() => {
  runsTimer = setInterval(async () => {
    runsData.value = await $fetch('/api/runs').catch(() => runsData.value)
  }, 60000)
})
onUnmounted(() => { if (runsTimer) clearInterval(runsTimer) })

const OUTCOME: Record<string, { label: string; tone: string }> = {
  cleared: { label: 'CLEARED', tone: T.green },
  partial: { label: 'PARTIAL', tone: 'oklch(0.80 0.10 88)' },
  fruitless: { label: 'NO KILLS', tone: V.faint },
  travel_timeout: { label: 'LOST ON ROAD', tone: T.red },
  door_timeout: { label: 'DIED AT DOOR', tone: T.red },
  enter_failed: { label: 'REFUSED ENTRY', tone: T.red },
  leader_lost: { label: 'LEADER LOST', tone: T.red },
  disbanded: { label: 'DISBANDED', tone: V.muted },
  wiped: { label: 'WIPED OUT', tone: T.red },
  underway: { label: 'UNDERWAY', tone: V.accentBright },
}

const runRows = computed(() =>
  (runsData.value?.runs ?? []).map((r: any) => {
    const o = OUTCOME[r.underway ? 'underway' : r.outcome]
      ?? { label: String(r.outcome).toUpperCase(), tone: V.muted }
    const bits: string[] = []
    if (r.total > 0) bits.push(`${r.downed}/${r.total} bosses`)
    if (r.wipes) bits.push(`${r.wipes} wipe${r.wipes === 1 ? '' : 's'}`)
    if (r.deaths) bits.push(`${r.deaths} deaths`)
    if (r.drops) bits.push(`${r.drops} drops`)
    if (r.mins) bits.push(`${r.mins}m`)
    return {
      id: r.id, ago: fmt.ago(r.started), dungeon: r.dungeon, isRaid: r.isRaid,
      heroic: r.heroic, size: r.size, avgIlvl: r.avgIlvl,
      leader: r.leader, leaderClass: r.leaderClass,
      label: o.label, tone: o.tone,
      detail: bits.join(' · ') || '—',
    }
  }))

const conversion = computed(() => {
  const c = props.assembler?.cycle
  return c?.trips ? Math.round((c.entered / c.trips) * 100) : null
})

const formingCount = computed(() =>
  (props.assembler?.funnel ?? []).find((f: any) => f.key === 'forming')?.value ?? 0)

const stages = computed(() => {
  const of = (tone: string) => board.value.filter((b: any) => b.tone === tone).map(card)
  const conv = conversion.value
  return [
    {
      key: 'forming', label: 'FORMING', color: STAGE_COLOR.forming,
      count: formingCount.value, cards: [] as ReturnType<typeof card>[],
      // The mirror tables only hold parties on a trip, so forming ones are a count.
      foot: !props.assembler?.ready ? null
        : formingCount.value > 0
          ? `The assembler holds ${spell(formingCount.value)} ${formingCount.value === 1 ? 'party' : 'parties'} between trips; the mirror itemises them once they move.`
          : 'Every assembled party is already moving.',
    },
    { key: 'travel', label: 'TRAVELLING', color: STAGE_COLOR.travel, count: 0, cards: of('travel'), foot: null },
    {
      key: 'door', label: 'AT THE DOOR', color: STAGE_COLOR.door, count: 0, cards: of('door'),
      foot: conv !== null && conv < 100
        ? `The door is where trips die: ${100 - conv}% of parties that start never zone in.`
        : null,
    },
    { key: 'inside', label: 'INSIDE', color: STAGE_COLOR.inside, count: 0, cards: of('inside'), foot: null },
  ].map(s => ({ ...s, count: s.key === 'forming' ? s.count : s.cards.length }))
})

const shapeStrip = computed(() =>
  (props.assembler?.shapes ?? []).slice(0, 4).map((s: any) => ({ shape: s.shape, n: s.n })))

const cycle = computed(() => props.assembler?.cycle)
const doors = computed(() => props.assembler?.doors)
const bossBoard = computed(() => props.assembler?.bossBoard ?? [])

const lede = computed(() => {
  const a = props.assembler
  if (!a?.ready) return 'The assembler has not written a tick yet.'
  const total = a.totalGroups ?? 0
  if (!total) return 'No groups exist right now.'
  const raidsIn = board.value.filter((b: any) => b.tone === 'inside' && b.isRaid)
  const clearing = raidsIn.some((b: any) => (b.encounters ?? []).some((e: any) => e.killed))
  const clause = clearing ? ', and a raid is clearing'
    : raidsIn.length ? ', and a raid is inside' : ''
  return `${titled(spell(total))} group${total === 1 ? '' : 's'} exist right now${clause}. ` +
    `Read left to right: the assembler's whole pipeline, forming to fighting.`
})
</script>

<template>
  <section :style="{ display: 'flex', flexDirection: 'column', gap: '16px', padding: '18px 22px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${V.accentDim}`, paddingLeft: '13px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '16px',
        lineHeight: 1.4, color: V.textMid,
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1.35fr 1fr 1.15fr', gap: '14px', flex: 'none', alignItems: 'start' }">
      <div
        v-for="st in stages"
        :key="st.key"
        :style="{ display: 'flex', flexDirection: 'column', gap: '10px', minHeight: 0 }"
      >
        <div :style="{ borderTop: `2px solid ${st.color}`, padding: '9px 2px 0', display: 'flex', justifyContent: 'space-between', alignItems: 'baseline' }">
          <span :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '10px', letterSpacing: '.16em', color: st.color }">{{ st.label }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.faint }">{{ st.count }}</span>
        </div>

        <button
          v-for="c in st.cards"
          :key="c.key"
          class="pipe-card"
          :style="{
            appearance: 'none', textAlign: 'left', fontFamily: FONT.body,
            border: `1px solid ${c.raid ? V.lineAccent : V.line}`,
            background: V.panel, boxShadow: V.inset, padding: '10px 12px',
            cursor: 'pointer', position: 'relative', display: 'block', width: '100%',
          }"
          @click="open = open === c.key ? null : c.key"
        >
          <span :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline' }">
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: c.idColor }">{{ c.id }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: c.noteColor }">{{ c.note }}</span>
          </span>
          <span :style="{ display: 'flex', alignItems: 'center', gap: '6px', fontSize: '14px', color: V.textHi, marginTop: '3px' }">
            <span :style="{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ c.dest }}</span>
            <UiSkull v-if="c.heroic" :size="11" title="Heroic lockout" />
          </span>
          <span :style="{ display: 'block', fontFamily: FONT.mono, fontSize: '10px', color: V.faint, marginTop: '3px', letterSpacing: '.05em' }">
            {{ c.sub }}<template v-if="c.ledBy"> · led by
              <span
                class="pipe-name"
                :style="{ color: V.textMid, cursor: 'pointer' }"
                @click.stop="emit('select', c.ledBy)"
              >{{ c.ledBy }}</span></template>
          </span>

          <span v-if="c.pips.length" :style="{ display: 'flex', gap: '3px', marginTop: '6px', alignItems: 'center', flexWrap: 'wrap' }">
            <span
              v-for="(on, i) in c.pips"
              :key="i"
              :style="{
                width: '7px', height: '7px', flex: 'none',
                background: on ? T.green : 'transparent',
                border: `1px solid ${on ? T.green : V.pip}`,
              }"
            />
            <span :style="{ fontFamily: FONT.mono, fontSize: '9.5px', color: c.pipNoteColor, marginLeft: '5px' }">{{ c.pipNote }}</span>
          </span>

          <span v-if="c.downed" :style="{ display: 'block', fontSize: '11.5px', color: T.green, marginTop: '6px', lineHeight: 1.45 }">↓ {{ c.downed }}</span>
          <span v-if="c.loot" :style="{ display: 'block', fontSize: '11.5px', marginTop: '2px', lineHeight: 1.45, color: V.faint }">
            {{ c.loot }}<span :style="{ color: c.lootColor }">{{ c.lootItem }}</span>
          </span>

          <span
            v-if="c.progW"
            :style="{ position: 'absolute', left: 0, bottom: 0, height: '2px', width: c.progW, background: V.accent, opacity: 0.55 }"
          />

          <span
            v-if="open === c.key"
            :style="{ display: 'block', marginTop: '9px', borderTop: `1px solid ${V.lineFaint}`, paddingTop: '8px' }"
          >
            <span :style="{ display: 'block', fontFamily: FONT.display, fontWeight: 600, fontSize: '9px', letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase' }">Roster</span>
            <span
              v-for="m in c.members"
              :key="m.name"
              :style="{ display: 'flex', justifyContent: 'space-between', gap: '8px', padding: '2px 0', fontSize: '12px' }"
            >
              <span
                class="pipe-name"
                :style="{ color: CLASS_COLOR[m.cls] ?? V.textHi, cursor: 'pointer', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
                @click.stop="emit('select', m.name)"
              >{{ m.name }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: m.leader ? V.accentBright : V.faint, flex: 'none' }">
                {{ m.leader ? 'lead · ' : '' }}{{ m.role }} · {{ m.level }}
              </span>
            </span>

            <template v-if="c.encounterList.length">
              <span :style="{ display: 'block', fontFamily: FONT.display, fontWeight: 600, fontSize: '9px', letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase', marginTop: '7px' }">Bosses</span>
              <span
                v-for="b in c.encounterList"
                :key="b.name"
                :style="{ display: 'flex', justifyContent: 'space-between', gap: '8px', padding: '2px 0', fontSize: '11.5px' }"
              >
                <span :style="{ color: b.killed ? T.green : V.muted }">{{ b.killed ? '↓ ' : '' }}{{ b.name }}</span>
                <span :style="{ fontFamily: FONT.mono, fontSize: '9.5px', color: b.killed ? T.green : V.faint, flex: 'none' }">{{ b.killed ? 'downed' : 'alive' }}</span>
              </span>
            </template>

            <template v-if="c.dropsList.length">
              <span :style="{ display: 'block', fontFamily: FONT.display, fontWeight: 600, fontSize: '9px', letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase', marginTop: '7px' }">Drops</span>
              <span
                v-for="d in c.dropsList"
                :key="d.name"
                :style="{ display: 'block', padding: '2px 0', fontSize: '11.5px', color: d.color }"
              >{{ d.name }}</span>
            </template>
          </span>
        </button>

        <p v-if="st.foot" :style="{ margin: '2px 2px 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.5 }">{{ st.foot }}</p>
      </div>
    </div>

    <div :style="{ display: 'flex', alignItems: 'center', gap: '24px', borderTop: `1px solid ${V.lineSoft}`, paddingTop: '12px', flex: 'none', flexWrap: 'wrap' }">
      <span v-if="cycle" :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.muted }">
        since boot: <span :style="{ color: V.textHi }">{{ fmt.int(cycle.trips) }}</span> trips started ·
        <span :style="{ color: V.textHi }">{{ fmt.int(cycle.entered) }}</span> got in<template v-if="conversion !== null"> ·
        <span :style="{ color: T.green }">{{ conversion }}%</span></template>
      </span>
      <span v-if="shapeStrip.length" :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.muted }">
        party shapes:
        <template v-for="(s, i) in shapeStrip" :key="s.shape">
          <template v-if="i"> · </template><span :style="{ color: V.textHi }">{{ s.shape }}</span> ×{{ s.n }}
        </template>
      </span>
      <span v-if="doors" :style="{ marginLeft: 'auto', fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint }">
        doors indexed: {{ doors.entrances }} entrances · {{ doors.arrivalPoints }} arrivals · {{ doors.raidMaps }} raid maps
      </span>
    </div>

    <UiPanel
      v-if="bossBoard.length"
      cap="The boss board · realm lifetime"
      note="filled = ever downed · gold rows are raids"
      :style="{ flex: 'none' }"
    >
      <UiEncounters :rows="bossBoard" :columns="2" />
    </UiPanel>

    <UiPanel
      v-if="runRows.length"
      cap="Run history"
      note="every journey, two weeks kept"
      :style="{ flex: 'none' }"
    >
      <div
        v-for="r in runRows"
        :key="r.id"
        class="hv-row"
        :style="{
          display: 'grid',
          gridTemplateColumns: '52px minmax(140px, 1.4fr) 58px minmax(90px, 1fr) 92px 1fr',
          gap: '10px', alignItems: 'baseline', padding: '4px 0',
          borderBottom: `1px solid ${V.lineFaint}`, fontSize: '12px',
        }"
      >
        <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">{{ r.ago }}</span>
        <span :style="{ display: 'flex', alignItems: 'center', gap: '5px', minWidth: 0 }">
          <span :style="{ color: r.isRaid ? V.accentBright : V.text, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ r.dungeon }}</span>
          <UiSkull v-if="r.heroic" :size="10" />
        </span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.muted }">{{ r.isRaid ? 'R' : 'G' }}{{ r.size }} · {{ r.avgIlvl }}il</span>
        <span
          class="pipe-name"
          :style="{ color: CLASS_COLOR[r.leaderClass] ?? V.textMid, cursor: 'pointer', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
          @click="emit('select', r.leader)"
        >{{ r.leader }}</span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: r.tone }">{{ r.label }}</span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint, textAlign: 'right' }">{{ r.detail }}</span>
      </div>
      <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
        A run that zoned in is graded by what it killed; one that never did says
        where the journey died. Gear is the party average at formation.
      </p>
    </UiPanel>

    <div
      v-if="quests"
      :style="{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(300px, 1fr))', gap: '14px', alignItems: 'start', flex: 'none' }"
    >
      <QuestPanels :quests="quests" @select="emit('select', $event)" />
    </div>

  </section>
</template>

<style scoped>
.pipe-card:hover { border-color: var(--accD) !important; }
.pipe-name:hover { text-decoration: underline; }
</style>
