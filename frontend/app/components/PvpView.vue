<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, fmt, spell, titled } from '../theme'
import { CLASS_COLOR, zoneName } from '../data'
import UiPanel from './UiPanel.vue'
import UiBars from './UiBars.vue'
import UiSpark from './UiSpark.vue'

const props = defineProps<{ pvp: any | null }>()
const emit = defineEmits<{ select: [string] }>()

const FACTION = {
  alliance: 'oklch(0.68 0.13 250)',
  horde: 'oklch(0.62 0.17 25)',
} as const

const head = computed(() => props.pvp?.headline ?? {})
const bgs = computed(() => props.pvp?.battlegrounds ?? [])

const lede = computed(() => {
  const inBg = head.value.inBattlegrounds ?? 0
  const kills = head.value.killsToday ?? 0
  if (!inBg && !kills) return 'Nobody is fighting anybody.'
  const where = inBg
    ? `${titled(spell(bgs.value.length))} battleground${bgs.value.length === 1 ? '' : 's'} ${bgs.value.length === 1 ? 'is' : 'are'} running with ${fmt.int(inBg)} characters in them.`
    : 'No battleground is running.'
  return `${where} ${fmt.int(kills)} honourable kills have been scored today.`
})

const tempo = computed(() =>
  (props.pvp?.tempo ?? []).map((t: any) => ({ label: t.hour, value: t.kills, second: t.deaths })))

const killerRows = computed(() =>
  (props.pvp?.killers ?? []).map((k: any) => ({
    label: k.name, value: k.today,
    note: k.inBg ? 'in a bg' : zoneName(k.zone).toLowerCase().slice(0, 14),
    tone: CLASS_COLOR[k.cls] ?? T.gold,
  })))

const arenaTeams = computed(() => props.pvp?.arenaTeams ?? [])
</script>

<template>
  <section :style="{ display: 'grid', gridTemplateRows: 'auto 1fr', gap: '14px', padding: '20px 22px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${T.goldDim}`, paddingLeft: '15px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '17.5px',
        lineHeight: 1.4, color: T.textMid, maxWidth: '64ch',
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(300px, 1fr))', gap: '16px', alignItems: 'start' }">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Battlegrounds · live" :note="`${fmt.int(head.inBattlegrounds ?? 0)} in the field`">
          <p
            v-if="!bgs.length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >No battleground is running. Bots queue automatically only when
            <code :style="{ fontFamily: FONT.mono }">RandomBotAutoJoinBG</code> is on.</p>

          <div
            v-for="b in bgs"
            :key="b.map"
            :style="{ padding: '7px 0', borderBottom: `1px solid ${T.lineFaint}` }"
          >
            <div :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', gap: '10px' }">
              <span :style="{ fontSize: '14px', color: T.textHi }">{{ b.name }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: T.muted }">
                {{ b.chars }}/{{ b.cap }} · lvl {{ b.avgLevel }}
              </span>
            </div>

            <!-- Split by faction rather than one bar: which side is short is the thing
                 that decides whether a battleground pops at all. -->
            <div :style="{ display: 'flex', height: '8px', gap: '1px', marginTop: '5px' }">
              <span :style="{ flex: b.alliance || 0.001, background: FACTION.alliance }" />
              <span :style="{ flex: b.horde || 0.001, background: FACTION.horde }" />
              <span
                v-if="b.chars < b.cap"
                :style="{ flex: b.cap - b.chars, background: 'oklch(0.26 0.02 56)' }"
              />
            </div>
            <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">
              <span>{{ b.alliance }} alliance</span>
              <span>{{ b.horde }} horde</span>
            </div>
          </div>
        </UiPanel>

        <UiPanel v-if="pvp?.arenas?.length" cap="Arenas · live">
          <div
            v-for="a in pvp.arenas"
            :key="a.map"
            :style="{ display: 'flex', justifyContent: 'space-between', padding: '4px 0', fontSize: '13px' }"
          >
            <span :style="{ color: T.body }">{{ a.name }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.text }">{{ a.chars }}</span>
          </div>
        </UiPanel>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Conflict" note="realm totals">
          <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 14px' }">
            <div v-for="s in [
              { v: fmt.int(head.killsToday ?? 0), l: 'honourable kills today' },
              { v: fmt.int(head.killersToday ?? 0), l: 'characters killing' },
              { v: fmt.int(head.topHonor ?? 0), l: 'top honour held' },
              { v: fmt.int(head.withHonor ?? 0), l: 'have honour' },
            ]" :key="s.l">
              <div :style="{ fontFamily: FONT.mono, fontSize: '19px', color: T.textHi }">{{ s.v }}</div>
              <div :style="{ fontSize: '11.5px', color: T.muted }">{{ s.l }}</div>
            </div>
          </div>

          <!-- Where those kills are being scored. Battleground share is exact for anyone
               currently in one; the rest is everything else. -->
          <div :style="{ display: 'flex', height: '8px', gap: '1px', marginTop: '12px' }">
            <span :style="{ flex: head.killsInBg || 0.001, background: 'oklch(0.68 0.19 25)' }" />
            <span :style="{ flex: head.killsInWorld || 0.001, background: 'oklch(0.62 0.10 88)' }" />
          </div>
          <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '5px', fontSize: '12px', color: T.muted }">
            <span>{{ fmt.int(head.killsInBg ?? 0) }} in battlegrounds</span>
            <span>{{ fmt.int(head.killsInWorld ?? 0) }} out in the world</span>
          </div>

          <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.45 }">
            Player kills only — NPCs never count toward this. The counter is cumulative
            per character, so the split is by where each killer stands now.
          </p>
        </UiPanel>

        <UiPanel v-if="tempo.length" cap="Conflict tempo" note="12h">
          <UiSpark
            :points="tempo"
            hue="oklch(0.68 0.19 25)"
            second-hue="oklch(0.52 0.06 60)"
            label="honourable kills"
            second-label="deaths"
          />
        </UiPanel>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel v-if="killerRows.length" cap="Top killers" note="today">
          <UiBars :rows="killerRows" label-width="104px" />
        </UiPanel>

        <UiPanel cap="Match records" :note="pvp?.matchesRecorded ? 'stored' : 'not stored'">
          <p
            v-if="!pvp?.matchesRecorded"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >
            Battlegrounds are running, but per-match records are not being written.
            <code :style="{ fontFamily: FONT.mono }">Battleground.StoreStatistics.Enable</code>
            is off, so <code :style="{ fontFamily: FONT.mono }">pvpstats_battlegrounds</code>
            stays empty and scores, brackets and winners cannot be shown.
          </p>
          <div
            v-for="m in pvp?.matches ?? []"
            :key="m.id"
            :style="{ display: 'flex', justifyContent: 'space-between', padding: '4px 0', fontSize: '13px' }"
          >
            <span :style="{ color: T.body }">match #{{ m.id }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: m.winner ? FACTION.horde : FACTION.alliance }">
              {{ m.winner ? 'horde' : 'alliance' }}
            </span>
          </div>
        </UiPanel>

        <UiPanel v-if="arenaTeams.length" cap="Arena teams" note="rated">
          <div
            v-for="t in arenaTeams"
            :key="t.id"
            :style="{ display: 'grid', gridTemplateColumns: '1fr auto auto', gap: '10px', padding: '4px 0', alignItems: 'baseline' }"
          >
            <span :style="{ fontSize: '13px', color: T.body, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ t.name }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">{{ t.size }}v{{ t.size }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.text }">{{ t.rating }}</span>
          </div>
        </UiPanel>
      </div>
    </div>
  </section>
</template>
