<script setup lang="ts">
import { computed, ref } from 'vue'
import { T, FONT, fmt, spell, titled } from '../theme'
import { CLASS_COLOR, CLASSES } from '../data'
import UiPanel from './UiPanel.vue'
import UiBars from './UiBars.vue'
import QuestPanels from './QuestPanels.vue'

type Member = {
  guid: number; name: string; cls: number; level: number; leader: boolean; role: string
}
type Encounter = { name: string; killed: boolean }

type BoardRow = {
  id: number; label: string; isRaid: boolean; leader: string; size: string
  levels: string; dest: string; via: string; remaining: string; status: string
  tone: string; pct: number; viaPlace: string | null; viaActor: string | null
  leaderClass: number; members: Member[]; encounters: Encounter[]
  dwellMins: number | null; bosses: number; deaths: number; looted: number
}

const props = defineProps<{ assembler: any | null; quests: any | null }>()
const emit = defineEmits<{ select: [string] }>()

const TONE: Record<string, string> = {
  travel: T.gold,
  door: 'oklch(0.82 0.08 350)',
  inside: T.green,
  forming: T.dim,
}

const ROLE_MARK: Record<string, string> = { tank: '▣', healer: '✚', dps: '✦' }
const ROLE_COLOR: Record<string, string> = {
  tank: 'oklch(0.72 0.06 250)', healer: T.green, dps: 'oklch(0.70 0.02 70)',
}

const board = computed<BoardRow[]>(() => props.assembler?.board ?? [])

// One row open at a time. Showing every roster and every boss list at once is what made
// this screen unreadable: thirty rows became ninety lines of competing colour. They are
// detail, and detail belongs behind a click.
const openId = ref<number | null>(null)
const toggle = (id: number) => { openId.value = openId.value === id ? null : id }

const funnel = computed(() => (props.assembler?.funnel ?? []).map((f: any) => ({
  ...f,
  color: TONE[f.key === 'door' ? 'door' : f.key === 'inside' ? 'inside'
    : f.key === 'travelling' ? 'travel' : 'forming'],
})))

const cycle = computed(() => props.assembler?.cycle)
const conversion = computed(() => {
  const c = cycle.value
  return c?.trips ? Math.round((c.entered / c.trips) * 100) : null
})

const lede = computed(() => {
  const a = props.assembler
  if (!a?.ready) return 'The assembler has not written a tick yet.'
  const total = a.totalGroups ?? 0
  const inside = board.value.filter(b => b.tone === 'inside').length
  if (!total) return 'No groups exist right now.'
  return `${titled(spell(total))} group${total === 1 ? '' : 's'} exist right now. ` +
    (inside ? `${titled(spell(inside))} ${inside === 1 ? 'is' : 'are'} through a door.`
            : 'None have made it through a door yet.')
})

// Boss progress as pips rather than names: Dire Maul alone printed sixteen names into a
// table row. Pips carry "how far in" at a glance; the names are one click away.
const MAX_PIPS = 12

// Clear rate per dungeon, and where the pipeline actually spends its time. This is the
// panel for deciding whether FootRange, StallTicks or InsideTicks need moving.
const clearRate = computed(() =>
  (props.assembler?.clearRate ?? []).map((c: any) => ({
    label: c.dungeon, value: c.pct,
    note: `${c.withKill}/${c.instances}`,
    // Green where content is actually falling, amber where it is being visited and not
    // beaten - the distinction the eight-hour measurement turned on.
    tone: c.pct >= 40 ? T.green : c.pct >= 15 ? T.gold : 'oklch(0.58 0.10 30)',
  })))

const demand = computed(() =>
  (props.assembler?.demand ?? []).map((d: any) => ({
    label: d.dungeon, value: d.entries, note: `lvl ${d.avgLevel}`,
  })))

const shapes = computed(() =>
  (props.assembler?.shapes ?? []).map((s: any) => ({
    label: `${s.shape} T/H/D`, value: s.n,
  })))

const clock = computed(() => props.assembler?.clock ?? [])

const raids = computed(() => props.assembler?.raids ?? { formed: 0, inFlight: 0, configuredPct: 0, actualPct: 0 })
const raidEntries = computed(() =>
  (props.assembler?.raids?.entries ?? []).map((r: any) => ({
    label: r.raid, value: r.entries, note: `${r.bots} bots`,
  })))
const raidBossTotal = computed(() =>
  (props.assembler?.raids?.instances ?? []).reduce((n: number, r: any) => n + r.bosses, 0))
const COLUMNS = '92px 44px minmax(140px, 1fr) 84px 116px 82px 92px'
</script>

<template>
  <section :style="{ display: 'flex', flexDirection: 'column', gap: '14px', padding: '20px 22px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${T.goldDim}`, paddingLeft: '15px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '17.5px',
        lineHeight: 1.4, color: T.textMid, maxWidth: '64ch',
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'grid', gridTemplateColumns: 'minmax(206px, 224px) 1fr', gap: '16px', alignItems: 'start', minHeight: '460px', flex: 'none' }">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Assembly funnel · live">
          <!-- A distribution reads faster as proportions than as a column of numbers. -->
          <div :style="{ display: 'flex', height: '8px', gap: '1px', marginBottom: '10px' }">
            <span
              v-for="f in funnel"
              :key="f.key"
              :style="{ flex: `${f.value || 0} 0 auto`, minWidth: f.value ? '3px' : '0', background: f.color }"
              :title="`${f.label}: ${f.value}`"
            />
          </div>
          <div
            v-for="f in funnel"
            :key="f.key"
            :style="{ display: 'flex', alignItems: 'baseline', gap: '8px', padding: '2px 0' }"
          >
            <span :style="{ width: '7px', height: '7px', background: f.color, flex: 'none' }" />
            <span :style="{ flex: 1, fontSize: '13px', color: T.body }">{{ f.label }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '12.5px', color: T.text }">{{ f.value }}</span>
          </div>

          <p
            v-if="cycle"
            :style="{
              margin: '10px 0 0', paddingTop: '9px', borderTop: `1px solid ${T.lineSoft}`,
              fontSize: '12.5px', color: T.muted, lineHeight: 1.45,
            }"
          >
            Since boot: {{ fmt.int(cycle.trips) }} started, {{ fmt.int(cycle.entered) }} got
            in<span v-if="conversion !== null"> — {{ conversion }}%</span>.
          </p>
        </UiPanel>

        <UiPanel v-if="clock.length" cap="Trip clock" note="tick budget burned">
          <div
            v-for="c in clock"
            :key="`${c.via}-${c.clock}`"
            :style="{ display: 'grid', gridTemplateColumns: '1fr auto', gap: '8px', padding: '4px 0', alignItems: 'baseline' }"
          >
            <span :style="{ fontSize: '12.5px', color: T.body }">
              {{ c.via.toLowerCase() }}
              <span :style="{ color: T.faint }">· {{ c.clock }}</span>
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: c.nearExpiry ? T.gold : T.muted }">
              {{ c.parties }}p · {{ c.avgTicks }}t<span v-if="c.nearExpiry"> · {{ c.nearExpiry }} expiring</span>
            </span>
          </div>
        </UiPanel>

        <UiPanel v-if="shapes.length" cap="Party shapes" note="tank / healer / dps">
          <UiBars :rows="shapes" label-width="92px" />
        </UiPanel>

        <UiPanel cap="Doors indexed">
          <div :style="{ display: 'flex', gap: '16px' }">
            <div v-for="d in [
              { n: assembler?.doors?.entrances ?? 0, l: 'entrances' },
              { n: assembler?.doors?.arrivalPoints ?? 0, l: 'arrivals' },
              { n: assembler?.doors?.raidMaps ?? 0, l: 'raid maps' },
            ]" :key="d.l">
              <div :style="{ fontFamily: FONT.mono, fontSize: '18px', color: T.textHi }">{{ d.n }}</div>
              <div :style="{ fontSize: '11.5px', color: T.muted }">{{ d.l }}</div>
            </div>
          </div>
        </UiPanel>

        <!-- Reference data, not a live signal. Folded away so it stops competing with the
             funnel for attention. -->
        <details :style="{ border: `1px solid ${T.line}`, background: T.panel, boxShadow: T.inset }">
          <summary
            :style="{
              cursor: 'pointer', padding: '10px 12px', fontFamily: FONT.display,
              fontWeight: 600, fontSize: '10px', letterSpacing: '.16em', color: T.dim,
              textTransform: 'uppercase',
            }"
          >Assembler settings</summary>
          <div :style="{ padding: '0 12px 12px' }">
            <div
              v-for="k in assembler?.config ?? []"
              :key="k.k"
              :style="{ display: 'flex', justifyContent: 'space-between', gap: '10px', padding: '2px 0', fontFamily: FONT.mono, fontSize: '10.5px' }"
            >
              <span :style="{ color: T.muted }">{{ k.k }}</span>
              <span :style="{ color: T.text }">{{ k.v }}</span>
            </div>
            <p :style="{ margin: '8px 0 0', fontSize: '11px', color: T.faint, lineHeight: 1.4 }">
              Read from the live playerbots.conf.
            </p>
          </div>
        </details>
      </div>

      <UiPanel
        cap="Groups in flight"
        :note="`${fmt.int(assembler?.activeParties ?? 0)} assembler · ${fmt.int(assembler?.totalGroups ?? 0)} total`"
        flush
        fill
        :style="{ maxHeight: '620px' }"
      >
        <div
          :style="{
            display: 'grid', gridTemplateColumns: COLUMNS, gap: '12px',
            padding: '0 14px 8px', borderBottom: `1px solid ${T.line}`,
            fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px',
            letterSpacing: '.14em', color: 'oklch(0.55 0.03 68)', minWidth: '690px',
          }"
        >
          <div>GROUP</div><div>SIZE</div><div>DESTINATION</div><div>VIA</div>
          <div>PROGRESS</div><div>REMAINING</div><div>STATUS</div>
        </div>

        <p
          v-if="!board.length"
          :style="{ padding: '16px 14px', margin: 0, color: T.muted, fontSize: '13.5px' }"
        >No party is travelling right now.</p>

        <div v-for="p in board" :key="p.id" :style="{ minWidth: '690px' }">
          <button
            :style="{
              width: '100%', textAlign: 'left', appearance: 'none', border: 'none',
              borderBottom: `1px solid ${T.lineFaint}`,
              background: openId === p.id ? 'oklch(0.225 0.022 54)' : 'transparent',
              cursor: 'pointer', padding: 0, display: 'block', position: 'relative',
            }"
            @click="toggle(p.id)"
          >
            <span :style="{ display: 'grid', gridTemplateColumns: COLUMNS, gap: '12px', padding: '9px 14px', alignItems: 'center' }">
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: p.isRaid ? T.gold : T.muted }">
                {{ p.label }}
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: T.muted }">{{ p.size }}</span>
              <span :style="{ fontSize: '14px', color: T.textHi, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
                {{ p.dest }}
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '9.5px', letterSpacing: '.08em', color: T.muted }">
                {{ p.via }}
              </span>

              <!-- Filled pip means down. A dash when the encounter table has no rows for
                   this map, rather than implying zero progress. -->
              <span :style="{ display: 'flex', alignItems: 'center', gap: '3px' }">
                <template v-if="p.encounters?.length">
                  <span
                    v-for="(b, i) in p.encounters.slice(0, MAX_PIPS)"
                    :key="i"
                    :style="{
                      width: '7px', height: '7px', flex: 'none',
                      background: b.killed ? T.green : 'transparent',
                      border: `1px solid ${b.killed ? T.green : 'oklch(0.42 0.03 62)'}`,
                    }"
                    :title="`${b.name} — ${b.killed ? 'down' : 'alive'}`"
                  />
                  <span
                    v-if="p.encounters.length > MAX_PIPS"
                    :style="{ fontFamily: FONT.mono, fontSize: '9.5px', color: T.faint }"
                  >+{{ p.encounters.length - MAX_PIPS }}</span>
                </template>
                <span v-else :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">—</span>
              </span>

              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.muted }">
                {{ p.tone === 'inside' ? (p.dwellMins != null ? `${p.dwellMins}m in` : '—') : p.remaining }}
              </span>
              <span :style="{ display: 'flex', alignItems: 'center', gap: '6px' }">
                <span :style="{ width: '5px', height: '5px', borderRadius: '50%', background: TONE[p.tone], flex: 'none' }" />
                <span :style="{ fontFamily: FONT.mono, fontSize: '9.5px', letterSpacing: '.09em', color: TONE[p.tone] }">
                  {{ p.status }}
                </span>
              </span>
            </span>
            <span :style="{ position: 'absolute', left: 0, bottom: 0, height: '2px', width: `${p.pct}%`, background: TONE[p.tone], opacity: 0.5 }" />
          </button>

          <div
            v-if="openId === p.id"
            :style="{
              padding: '13px 14px 15px 106px', borderBottom: `1px solid ${T.lineFaint}`,
              background: 'oklch(0.205 0.02 54)', display: 'flex', flexWrap: 'wrap', gap: '26px',
            }"
          >
            <div :style="{ minWidth: '200px' }">
              <div :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px', letterSpacing: '.14em', color: T.dim, marginBottom: '7px' }">
                PARTY · LEVELS {{ p.levels }}
              </div>
              <button
                v-for="m in p.members"
                :key="m.guid"
                :style="{
                  appearance: 'none', background: 'none', border: 'none', padding: '2px 0',
                  cursor: 'pointer', display: 'flex', alignItems: 'baseline', gap: '7px',
                  width: '100%', textAlign: 'left',
                }"
                :title="`${CLASSES[m.cls] ?? 'unknown'} · ${m.role}`"
                @click.stop="emit('select', m.name)"
              >
                <span :style="{ fontSize: '9px', color: ROLE_COLOR[m.role] ?? T.faint, width: '10px' }">
                  {{ ROLE_MARK[m.role] ?? '·' }}
                </span>
                <span :style="{ fontSize: '13px', color: CLASS_COLOR[m.cls] ?? T.text, fontWeight: m.leader ? 600 : 400 }">
                  {{ m.name }}
                </span>
                <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">{{ m.level }}</span>
              </button>
            </div>

            <div v-if="p.encounters?.length" :style="{ minWidth: '200px' }">
              <div :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px', letterSpacing: '.14em', color: T.dim, marginBottom: '7px' }">
                ENCOUNTERS · {{ p.encounters.filter(b => b.killed).length }} OF {{ p.encounters.length }}
              </div>
              <div
                v-for="b in p.encounters"
                :key="b.name"
                :style="{
                  fontSize: '13px', padding: '2px 0',
                  color: b.killed ? T.green : T.muted,
                  textDecoration: b.killed ? 'line-through' : 'none',
                }"
              >{{ b.name }}</div>
            </div>

            <div :style="{ minWidth: '150px' }">
              <div :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px', letterSpacing: '.14em', color: T.dim, marginBottom: '7px' }">
                THIS RUN
              </div>
              <div :style="{ fontSize: '13px', color: T.body, lineHeight: 1.65 }">
                <div v-if="p.viaPlace">{{ titled(p.via.toLowerCase()) }} to {{ p.viaPlace }}</div>
                <div v-if="p.viaActor">portal by {{ p.viaActor }}</div>
                <div>{{ p.deaths }} death{{ p.deaths === 1 ? '' : 's' }} · {{ p.looted }} drop{{ p.looted === 1 ? '' : 's' }}</div>
              </div>
            </div>
          </div>
        </div>
      </UiPanel>
    </div>

    <div
      v-if="clearRate.length || demand.length || quests"
      :style="{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(300px, 1fr))', gap: '16px', alignItems: 'start', flex: 'none' }"
    >
      <UiPanel v-if="clearRate.length" cap="Clear rate by dungeon" note="instances with a boss down">
        <UiBars :rows="clearRate" :max="100" unit="%" label-width="140px" />
        <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.45 }">
          Only dungeons with three or more runs behind them, so one lucky party does not
          read as a perfect record.
        </p>
      </UiPanel>

      <UiPanel v-if="demand.length" cap="Dungeon demand" note="entries recorded">
        <UiBars :rows="demand" label-width="140px" hue="oklch(0.70 0.12 240)" />
      </UiPanel>

      <QuestPanels :quests="quests" @select="emit('select', $event)" />

      <UiPanel
        v-if="raidEntries.length"
        cap="Raids"
        :note="`${raids.formed} formed · ${raids.inFlight} in flight`"
      >
        <UiBars :rows="raidEntries" label-width="140px" hue="oklch(0.72 0.14 305)" />
        <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.45 }">
          Entries over 24h, matched by map id — name matching would fold Uldaman, a
          five-man, into Zul'Aman.
          <span v-if="raidBossTotal === 0" :style="{ color: T.gold }">
            No boss has died in a raid yet: they form, travel and zone in, then clear
            nothing.
          </span>
          <span v-if="raids.actualPct < raids.configuredPct - 3">
            RaidPct asks for {{ raids.configuredPct }}% of parties; {{ raids.actualPct }}%
            get through, because a raid only forms when one is reachable at the leader's
            level on their own continent.
          </span>
        </p>
      </UiPanel>
    </div>
  </section>
</template>
