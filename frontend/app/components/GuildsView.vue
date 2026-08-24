<script setup lang="ts">
import { computed, ref } from 'vue'
import { T, FONT, V, fmt, spell, titled } from '../theme'
import { CLASS_COLOR } from '../data'

const props = defineProps<{ guild: any | null }>()
const emit = defineEmits<{ select: [string] }>()

// Battleground faction colors are data, not chrome - they never follow the palette.
const FACTION = {
  alliance: 'oklch(0.68 0.13 250)',
  horde: 'oklch(0.62 0.17 25)',
} as const

const panel = { border: `1px solid ${V.line}`, background: V.panel, boxShadow: V.inset }
const cap = {
  fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
  letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase' as const,
}
const capMini = {
  fontFamily: FONT.display, fontWeight: 600, fontSize: '9px',
  letterSpacing: '.14em', color: V.dim, marginBottom: '7px',
}
const COLS = '1fr 66px 58px 52px 74px'

type Sort = 'level' | 'members' | 'cap' | 'hour'
const sort = ref<Sort>('level')
const SORTS: { key: Sort; label: string }[] = [
  { key: 'level', label: 'AVG LEVEL' },
  { key: 'members', label: 'SIZE' },
  { key: 'cap', label: 'AT CAP' },
  { key: 'hour', label: 'ACTIVE NOW' },
]

// One detail row open at a time; a second click on the same row closes it.
const openId = ref<number | null>(null)

const guilds = computed(() => {
  const by: Record<Sort, (a: any, b: any) => number> = {
    level: (a, b) => b.avgLevel - a.avgLevel,
    members: (a, b) => b.members - a.members,
    cap: (a, b) => b.atCap - a.atCap,
    hour: (a, b) => b.activity.events - a.activity.events,
  }
  return [...(props.guild?.guilds ?? [])].sort(by[sort.value])
})

const totals = computed(() => props.guild?.totals ?? null)

const lede = computed(() => {
  const n = props.guild?.total ?? 0
  if (!n) return 'No guilds have formed yet.'
  const active = totals.value?.activeLastHour ?? 0
  return `${titled(spell(n))} guilds have formed without anyone asking them to. ` +
    `${active ? `${titled(spell(active))} did something in the last hour.` : 'None have stirred in the last hour.'}`
})

// Recorded events are the only real activity signal, so they rank the busy list.
const busiest = computed(() => {
  const rows = [...(props.guild?.guilds ?? [])]
    .filter(g => g.activity.events > 0)
    .sort((a, b) => b.activity.events - a.activity.events)
    .slice(0, 10)
  const max = Math.max(1, ...rows.map(g => g.activity.events))
  return rows.map(g => ({
    name: g.name,
    v: g.activity.events,
    note: `${g.activity.active} bots`,
    c: FACTION[g.faction as keyof typeof FACTION],
    w: `${Math.round((g.activity.events / max) * 100)}%`,
  }))
})

const factionSplit = computed(() => {
  const out = { alliance: 0, horde: 0 }
  for (const g of props.guild?.guilds ?? []) out[g.faction as 'alliance' | 'horde'] += g.members
  return out
})

const plur = (n: number, word: string) => `${n} ${word}${n === 1 ? '' : 's'}`

const lastHour = (a: any) => {
  if (!a.events) return 'Nothing recorded.'
  const parts = [`${a.active} active`]
  if (a.instances) parts.push(plur(a.instances, 'instance move'))
  if (a.loot) parts.push(plur(a.loot, 'drop'))
  if (a.levels) parts.push(`${a.levels} level-up${a.levels === 1 ? '' : 's'}`)
  if (a.deaths) parts.push(plur(a.deaths, 'death'))
  return parts.join(' · ')
}
</script>

<template>
  <section :style="{ padding: '18px 22px', display: 'flex', flexDirection: 'column', gap: '16px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${V.accentDim}`, paddingLeft: '13px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '16px',
        lineHeight: 1.4, color: V.textMid,
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'grid', gridTemplateColumns: '280px 1fr', gap: '16px', alignItems: 'start', minHeight: 0 }">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <section :style="panel">
          <header :style="{ padding: '10px 12px 6px' }"><span :style="cap">Realm-wide</span></header>
          <div :style="{ padding: '0 12px 12px' }">
            <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 14px' }">
              <div v-for="s in [
                { v: fmt.int(guild?.total ?? 0), l: 'guilds' },
                { v: fmt.int(totals?.members ?? 0), l: 'members' },
                { v: fmt.int(totals?.atCap ?? 0), l: 'at level 80' },
                { v: fmt.int(totals?.activeLastHour ?? 0), l: 'active this hour' },
              ]" :key="s.l">
                <div :style="{ fontFamily: FONT.mono, fontSize: '19px', color: V.textHi }">{{ s.v }}</div>
                <div :style="{ fontSize: '11.5px', color: V.muted }">{{ s.l }}</div>
              </div>
            </div>

            <div :style="{ display: 'flex', height: '8px', gap: '1px', marginTop: '12px' }">
              <span :style="{ flex: factionSplit.alliance || 1, background: FACTION.alliance }" />
              <span :style="{ flex: factionSplit.horde || 1, background: FACTION.horde }" />
            </div>
            <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '5px', fontSize: '12px', color: V.muted }">
              <span>Alliance {{ fmt.int(factionSplit.alliance) }}</span>
              <span>Horde {{ fmt.int(factionSplit.horde) }}</span>
            </div>
          </div>
        </section>

        <section v-if="busiest.length" :style="panel">
          <header :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', padding: '10px 12px 6px' }">
            <span :style="cap">Busiest this hour</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">recorded events</span>
          </header>
          <div :style="{ padding: '0 12px 12px' }">
            <div
              v-for="g in busiest"
              :key="g.name"
              :style="{ display: 'grid', gridTemplateColumns: '110px 1fr auto auto', gap: '9px', alignItems: 'center', padding: '3px 0' }"
            >
              <span :style="{ fontSize: '12.5px', color: V.body, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }" :title="g.name">{{ g.name }}</span>
              <span :style="{ height: '7px', background: V.track, position: 'relative' }">
                <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: g.w, background: g.c }" />
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.text }">{{ g.v }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">{{ g.note }}</span>
            </div>
          </div>
        </section>

        <!-- Said plainly rather than rendered as an empty panel. -->
        <section v-if="(totals?.withBosses ?? 0) === 0" :style="panel">
          <header :style="{ padding: '10px 12px 6px' }"><span :style="cap">Guild progression</span></header>
          <p :style="{ margin: 0, padding: '0 12px 12px', fontSize: '13px', color: V.muted, lineHeight: 1.5 }">
            No guild has a boss attributed to it. Kills are only attributable through
            instance bindings, and five-man runs do not create one — so this fills in for
            raid content, not dungeons.
          </p>
        </section>
      </div>

      <section :style="panel">
        <header :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', padding: '10px 14px 8px' }">
          <span :style="cap">Guilds</span>
          <span :style="{ display: 'flex', gap: '1px' }">
            <button
              v-for="s in SORTS"
              :key="s.key"
              :style="{
                appearance: 'none', cursor: 'pointer', padding: '4px 9px',
                background: sort === s.key ? V.raised : 'transparent',
                border: `1px solid ${sort === s.key ? V.accentDim : V.line}`,
                color: sort === s.key ? V.accentBright : V.muted,
                fontFamily: FONT.mono, fontSize: '9.5px', letterSpacing: '.05em',
              }"
              @click="sort = s.key"
            >{{ s.label }}</button>
          </span>
        </header>

        <div
          :style="{
            display: 'grid', gridTemplateColumns: COLS, gap: '12px',
            padding: '0 14px 8px', borderBottom: `1px solid ${V.line}`,
            fontFamily: FONT.display, fontWeight: 600, fontSize: '9px',
            letterSpacing: '.14em', color: V.faint2,
          }"
        >
          <span>GUILD</span><span>MEMBERS</span><span>AVG LVL</span><span>AT CAP</span><span>THIS HOUR</span>
        </div>

        <div v-for="g in guilds" :key="g.id">
          <button
            :class="openId === g.id ? '' : 'hv-raised'"
            :style="{
              appearance: 'none', width: '100%', textAlign: 'left', border: 'none',
              borderBottom: `1px solid ${V.lineFaint}`,
              ...(openId === g.id ? { background: V.raisedHi } : {}),
              cursor: 'pointer', padding: 0, display: 'block', fontFamily: FONT.body,
            }"
            @click="openId = openId === g.id ? null : g.id"
          >
            <span :style="{ display: 'grid', gridTemplateColumns: COLS, gap: '12px', padding: '8px 14px', alignItems: 'center' }">
              <span :style="{ display: 'flex', alignItems: 'center', gap: '8px', minWidth: 0 }">
                <span :style="{ width: '3px', height: '14px', background: FACTION[g.faction as 'alliance'|'horde'], flex: 'none' }" />
                <span :style="{ fontSize: '14px', color: V.textHi, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
                  {{ g.name }}
                </span>
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.muted }">{{ g.members }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.text }">{{ g.avgLevel }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: g.atCap ? V.accent : V.faint }">{{ g.atCap }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: g.activity.events ? T.green : V.faint }">
                {{ g.activity.events || '—' }}
              </span>
            </span>
          </button>

          <div
            v-if="openId === g.id"
            :style="{
              padding: '13px 14px 15px 25px', borderBottom: `1px solid ${V.lineFaint}`,
              background: V.panelOpen, display: 'flex', flexWrap: 'wrap', gap: '26px',
            }"
          >
            <div :style="{ minWidth: '180px' }">
              <div :style="capMini">ROSTER</div>
              <div :style="{ fontSize: '13px', color: V.body, lineHeight: 1.7 }">
                <div v-if="g.master">
                  led by
                  <button
                    class="nm"
                    :style="{
                      appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer',
                      fontFamily: FONT.body, fontSize: '13px', color: CLASS_COLOR[g.masterClass] ?? V.textHi,
                    }"
                    @click.stop="emit('select', g.master)"
                  >{{ g.master }}</button>
                </div>
                <div>{{ g.members }} members · {{ g.atCap }} at cap</div>
                <div>{{ fmt.int(g.gold) }}g held</div>
              </div>
            </div>

            <div :style="{ minWidth: '200px' }">
              <div :style="capMini">LAST HOUR</div>
              <div :style="{ fontSize: '13px', color: V.body, lineHeight: 1.7 }">{{ lastHour(g.activity) }}</div>
            </div>

            <div v-if="g.bosses?.length" :style="{ minWidth: '180px' }">
              <div :style="capMini">BOSSES DOWN</div>
              <div :style="{ fontSize: '13px', color: T.green, lineHeight: 1.7 }">
                <div v-for="b in g.bosses" :key="`${b.boss}-${b.instance}`">{{ b.boss }} — {{ b.instance }}</div>
              </div>
            </div>
          </div>
        </div>
      </section>
    </div>
  </section>
</template>

<style scoped>
.nm:hover { text-decoration: underline }
</style>
