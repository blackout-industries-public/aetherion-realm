<script setup lang="ts">
import { computed, ref } from 'vue'
import { T, FONT, fmt, spell, titled } from '../theme'
import UiPanel from './UiPanel.vue'
import UiBars from './UiBars.vue'

const props = defineProps<{ guild: any | null }>()
const emit = defineEmits<{ select: [string] }>()

const FACTION = {
  alliance: 'oklch(0.68 0.13 250)',
  horde: 'oklch(0.62 0.17 25)',
} as const

type Sort = 'level' | 'members' | 'activity' | 'cap'
const sort = ref<Sort>('level')
const SORTS: { key: Sort; label: string }[] = [
  { key: 'level', label: 'Avg level' },
  { key: 'members', label: 'Size' },
  { key: 'cap', label: 'At cap' },
  { key: 'activity', label: '活 Active now' },
]

const openId = ref<number | null>(null)

const guilds = computed(() => {
  const list = [...(props.guild?.guilds ?? [])]
  const by: Record<Sort, (a: any, b: any) => number> = {
    level: (a, b) => b.avgLevel - a.avgLevel,
    members: (a, b) => b.members - a.members,
    cap: (a, b) => b.atCap - a.atCap,
    activity: (a, b) => b.activity.events - a.activity.events,
  }
  return list.sort(by[sort.value])
})

const totals = computed(() => props.guild?.totals ?? null)

const lede = computed(() => {
  const n = props.guild?.total ?? 0
  if (!n) return 'No guilds have formed yet.'
  const active = totals.value?.activeLastHour ?? 0
  return `${titled(spell(n))} guilds have formed without anyone asking them to. ` +
    `${active ? `${titled(spell(active))} did something in the last hour.` : 'None have stirred in the last hour.'}`
})

// The busiest guilds by recorded events, which is the only real activity signal.
const activityRows = computed(() =>
  [...(props.guild?.guilds ?? [])]
    .filter(g => g.activity.events > 0)
    .sort((a, b) => b.activity.events - a.activity.events)
    .slice(0, 10)
    .map(g => ({
      label: g.name,
      value: g.activity.events,
      note: `${g.activity.active} bots`,
      tone: FACTION[g.faction as keyof typeof FACTION],
    })))

const factionSplit = computed(() => {
  const out = { alliance: 0, horde: 0 }
  for (const g of props.guild?.guilds ?? []) out[g.faction as 'alliance' | 'horde'] += g.members
  return out
})
</script>

<template>
  <section :style="{ display: 'grid', gridTemplateRows: 'auto 1fr', gap: '14px', padding: '20px 22px', minHeight: 0, height: '100%', overflow: 'hidden' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${T.goldDim}`, paddingLeft: '15px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '17.5px',
        lineHeight: 1.4, color: T.textMid, maxWidth: '64ch',
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'grid', gridTemplateColumns: 'minmax(230px, 268px) 1fr', gap: '16px', minHeight: 0 }">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px', minHeight: 0, overflow: 'auto' }">
        <UiPanel cap="Realm-wide">
          <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 14px' }">
            <div v-for="s in [
              { v: fmt.int(guild?.total ?? 0), l: 'guilds' },
              { v: fmt.int(totals?.members ?? 0), l: 'members' },
              { v: fmt.int(totals?.atCap ?? 0), l: 'at level 80' },
              { v: fmt.int(totals?.activeLastHour ?? 0), l: 'active this hour' },
            ]" :key="s.l">
              <div :style="{ fontFamily: FONT.mono, fontSize: '19px', color: T.textHi }">{{ s.v }}</div>
              <div :style="{ fontSize: '11.5px', color: T.muted }">{{ s.l }}</div>
            </div>
          </div>

          <div :style="{ display: 'flex', height: '8px', gap: '1px', marginTop: '12px' }">
            <span :style="{ flex: factionSplit.alliance || 1, background: FACTION.alliance }" />
            <span :style="{ flex: factionSplit.horde || 1, background: FACTION.horde }" />
          </div>
          <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '5px', fontSize: '12px', color: T.muted }">
            <span>Alliance {{ fmt.int(factionSplit.alliance) }}</span>
            <span>Horde {{ fmt.int(factionSplit.horde) }}</span>
          </div>
        </UiPanel>

        <UiPanel v-if="activityRows.length" cap="Busiest this hour" note="recorded events">
          <UiBars :rows="activityRows" label-width="104px" />
        </UiPanel>

        <!-- Said plainly rather than rendered as an empty panel. -->
        <UiPanel v-if="(totals?.withBosses ?? 0) === 0" cap="Guild progression">
          <p :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }">
            No guild has a boss attributed to it. Kills are only attributable through
            instance bindings, and five-man runs do not create one — so this fills in for
            raid content, not dungeons.
          </p>
        </UiPanel>
      </div>

      <UiPanel cap="Guilds" :note="`${guilds.length} · sorted by ${SORTS.find(s => s.key === sort)?.label.toLowerCase()}`" flush fill>
        <div :style="{ display: 'flex', gap: '1px', padding: '0 14px 9px' }">
          <button
            v-for="s in SORTS"
            :key="s.key"
            :style="{
              appearance: 'none', cursor: 'pointer', padding: '4px 9px',
              background: sort === s.key ? T.raised : 'transparent',
              border: `1px solid ${sort === s.key ? T.goldDim : T.line}`,
              color: sort === s.key ? T.goldBright : T.muted,
              fontFamily: FONT.mono, fontSize: '10px', letterSpacing: '.05em',
            }"
            @click="sort = s.key"
          >{{ s.label.replace('活 ', '') }}</button>
        </div>

        <div
          :style="{
            display: 'grid', gridTemplateColumns: '1fr 58px 58px 58px 74px', gap: '12px',
            padding: '0 14px 8px', borderBottom: `1px solid ${T.line}`,
            fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px',
            letterSpacing: '.14em', color: 'oklch(0.55 0.03 68)', minWidth: '560px',
          }"
        >
          <div>GUILD</div><div>MEMBERS</div><div>AVG LVL</div><div>AT CAP</div><div>THIS HOUR</div>
        </div>

        <div v-for="g in guilds" :key="g.id" :style="{ minWidth: '560px' }">
          <button
            :style="{
              width: '100%', textAlign: 'left', appearance: 'none', border: 'none',
              borderBottom: `1px solid ${T.lineFaint}`,
              background: openId === g.id ? 'oklch(0.225 0.022 54)' : 'transparent',
              cursor: 'pointer', padding: 0, display: 'block',
            }"
            @click="openId = openId === g.id ? null : g.id"
          >
            <span :style="{ display: 'grid', gridTemplateColumns: '1fr 58px 58px 58px 74px', gap: '12px', padding: '8px 14px', alignItems: 'center' }">
              <span :style="{ display: 'flex', alignItems: 'center', gap: '8px', minWidth: 0 }">
                <span :style="{ width: '3px', height: '14px', background: FACTION[g.faction as 'alliance'|'horde'], flex: 'none' }" />
                <span :style="{ fontSize: '14px', color: T.textHi, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
                  {{ g.name }}
                </span>
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.muted }">{{ g.members }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.text }">{{ g.avgLevel }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: g.atCap ? T.gold : T.faint }">{{ g.atCap }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: g.activity.events ? T.green : T.faint }">
                {{ g.activity.events || '—' }}
              </span>
            </span>
          </button>

          <div
            v-if="openId === g.id"
            :style="{
              padding: '13px 14px 15px 25px', borderBottom: `1px solid ${T.lineFaint}`,
              background: 'oklch(0.205 0.02 54)', display: 'flex', flexWrap: 'wrap', gap: '26px',
            }"
          >
            <div :style="{ minWidth: '190px' }">
              <div :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px', letterSpacing: '.14em', color: T.dim, marginBottom: '7px' }">
                ROSTER
              </div>
              <div :style="{ fontSize: '13px', color: T.body, lineHeight: 1.7 }">
                <div v-if="g.master">
                  led by
                  <button
                    :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', color: T.gold, fontSize: '13px' }"
                    @click.stop="emit('select', g.master)"
                  >{{ g.master }}</button>
                </div>
                <div>{{ g.members }} members · {{ g.online }} online</div>
                <div>levels up to {{ g.topLevel }} · {{ g.atCap }} at cap</div>
                <div>{{ fmt.int(g.gold) }}g held · {{ fmt.int(g.kills) }} honour kills</div>
              </div>
            </div>

            <div :style="{ minWidth: '190px' }">
              <div :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px', letterSpacing: '.14em', color: T.dim, marginBottom: '7px' }">
                LAST HOUR
              </div>
              <div v-if="g.activity.events" :style="{ fontSize: '13px', color: T.body, lineHeight: 1.7 }">
                <div>{{ g.activity.active }} members active</div>
                <div>{{ g.activity.instances }} instance moves · {{ g.activity.loot }} drops</div>
                <div>{{ g.activity.levels }} level-ups · {{ g.activity.deaths }} deaths</div>
              </div>
              <div v-else :style="{ fontSize: '13px', color: T.muted }">Nothing recorded.</div>
            </div>

            <div v-if="g.bosses?.length" :style="{ minWidth: '190px' }">
              <div :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px', letterSpacing: '.14em', color: T.dim, marginBottom: '7px' }">
                BOSSES DOWN
              </div>
              <div v-for="b in g.bosses" :key="b.boss" :style="{ fontSize: '13px', color: T.green, padding: '1px 0' }">
                {{ b.boss }}
                <span :style="{ color: T.faint, fontSize: '11px' }">{{ b.instance }}</span>
              </div>
            </div>
          </div>
        </div>
      </UiPanel>
    </div>
  </section>
</template>
