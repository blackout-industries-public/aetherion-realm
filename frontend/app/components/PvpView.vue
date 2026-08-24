<script setup lang="ts">
import { computed } from 'vue'
import { FONT, V, fmt, spell, titled } from '../theme'
import { CLASS_COLOR, zoneName } from '../data'
import UiPanel from './UiPanel.vue'

const props = defineProps<{ pvp: any | null }>()
const emit = defineEmits<{ select: [string] }>()

// Data colours, fixed across palettes: faction identity and the kill/death hues
// never follow the chrome accent.
const FACTION = {
  alliance: 'oklch(0.68 0.13 250)',
  horde: 'oklch(0.62 0.17 25)',
} as const
const KILL = 'oklch(0.68 0.19 25)'
const WORLD_KILL = 'oklch(0.62 0.10 88)'
const DEATH = 'oklch(0.52 0.06 60)'

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

const stats = computed(() => [
  { v: fmt.int(head.value.killsToday ?? 0), l: 'honourable kills today' },
  { v: fmt.int(head.value.killersToday ?? 0), l: 'characters killing' },
  { v: fmt.int(head.value.topHonor ?? 0), l: 'top honour held' },
  { v: fmt.int(head.value.withHonor ?? 0), l: 'have honour' },
])

const tempo = computed(() => props.pvp?.tempo ?? [])
const tempoPeak = computed(() =>
  Math.max(1, ...tempo.value.map((t: any) => Math.max(t.kills, t.deaths))))

const killers = computed(() => {
  const rows = props.pvp?.killers ?? []
  const max = Math.max(1, ...rows.map((k: any) => k.today))
  return rows.map((k: any) => ({
    name: k.name, value: k.today,
    note: k.inBg ? 'in a bg' : zoneName(k.zone).toLowerCase().slice(0, 14),
    color: CLASS_COLOR[k.cls] ?? V.textHi,
    w: `${Math.max(2, Math.round((k.today / max) * 100))}%`,
  }))
})
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

    <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(3, minmax(0, 1fr))', gap: '16px', alignItems: 'start' }">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Battlegrounds · live" :note="`${fmt.int(head.inBattlegrounds ?? 0)} in the field`">
          <p
            v-if="!bgs.length"
            :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
          >No battleground is running. Bots queue automatically only when
            <code :style="{ fontFamily: FONT.mono, fontSize: '11.5px' }">RandomBotAutoJoinBG</code> is on.</p>

          <div
            v-for="b in bgs"
            :key="b.map"
            :style="{ padding: '7px 0', borderBottom: `1px solid ${V.lineFaint}` }"
          >
            <div :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', gap: '10px' }">
              <span :style="{ fontSize: '14px', color: V.textHi }">{{ b.name }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.muted, fontVariantNumeric: 'tabular-nums' }">
                {{ b.chars }}/{{ b.cap }} · lvl {{ b.avgLevel }}
              </span>
            </div>

            <div :style="{ display: 'flex', height: '8px', gap: '1px', marginTop: '5px' }">
              <span :style="{ flex: b.alliance || 0.001, background: FACTION.alliance }" />
              <span :style="{ flex: b.horde || 0.001, background: FACTION.horde }" />
              <span
                v-if="b.chars < b.cap"
                :style="{ flex: b.cap - b.chars, background: V.track }"
              />
            </div>
            <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">
              <span>{{ b.alliance }} alliance</span>
              <span>{{ b.horde }} horde</span>
            </div>
          </div>

          <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
            Split by faction on purpose: which side is short decides whether a
            battleground pops at all.
          </p>
        </UiPanel>

        <UiPanel cap="Match records" :note="pvp?.matchesRecorded ? 'stored' : 'not stored'">
          <p
            v-if="!pvp?.matchesRecorded"
            :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
          >
            Battlegrounds run, but per-match records are not written.
            <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px' }">Battleground.StoreStatistics.Enable</span>
            is off, so scores, brackets and winners cannot be shown.
          </p>
          <div
            v-for="m in pvp?.matches ?? []"
            :key="m.id"
            :style="{ display: 'flex', justifyContent: 'space-between', padding: '4px 0', fontSize: '13px' }"
          >
            <span :style="{ color: V.body }">match #{{ m.id }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: m.winner ? FACTION.horde : FACTION.alliance }">
              {{ m.winner ? 'horde' : 'alliance' }}
            </span>
          </div>
        </UiPanel>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Conflict" note="realm totals">
          <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 14px' }">
            <div v-for="s in stats" :key="s.l">
              <div :style="{ fontFamily: FONT.mono, fontSize: '19px', color: V.textHi, fontVariantNumeric: 'tabular-nums' }">{{ s.v }}</div>
              <div :style="{ fontSize: '11.5px', color: V.muted }">{{ s.l }}</div>
            </div>
          </div>

          <div :style="{ display: 'flex', height: '8px', gap: '1px', marginTop: '12px' }">
            <span :style="{ flex: head.killsInBg || 0.001, background: KILL }" />
            <span :style="{ flex: head.killsInWorld || 0.001, background: WORLD_KILL }" />
          </div>
          <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '5px', fontSize: '12px', color: V.muted }">
            <span>{{ fmt.int(head.killsInBg ?? 0) }} in battlegrounds</span>
            <span>{{ fmt.int(head.killsInWorld ?? 0) }} out in the world</span>
          </div>

          <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
            Player kills only — NPCs never count. A kill credits every nearby
            group member, as on a character sheet.
          </p>
        </UiPanel>

        <!-- Inline rather than UiSpark: the design carries the peak in the header note
             and drops end labels, which the primitive cannot be told to do. -->
        <UiPanel v-if="tempo.length" cap="Conflict tempo" :note="`12h · peak ${fmt.int(tempoPeak)}`">
          <div :style="{ display: 'flex', alignItems: 'flex-end', gap: '2px', height: '54px' }">
            <span
              v-for="t in tempo"
              :key="t.hour"
              :style="{ flex: 1, position: 'relative', height: '100%', display: 'flex', alignItems: 'flex-end' }"
              :title="`${t.hour} · ${t.kills} kills / ${t.deaths} deaths`"
            >
              <span
                :style="{
                  position: 'absolute', left: 0, right: 0, bottom: 0,
                  height: `${(t.deaths / tempoPeak) * 100}%`,
                  background: DEATH, opacity: 0.55,
                }"
              />
              <span
                :style="{
                  position: 'relative', width: '100%',
                  height: `${(t.kills / tempoPeak) * 100}%`,
                  minHeight: t.kills > 0 ? '2px' : '0',
                  background: KILL,
                }"
              />
            </span>
          </div>
          <div :style="{ display: 'flex', alignItems: 'center', gap: '12px', marginTop: '6px' }">
            <span :style="{ display: 'flex', alignItems: 'center', gap: '5px' }">
              <span :style="{ width: '7px', height: '7px', background: KILL }" />
              <span :style="{ fontSize: '12px', color: V.muted }">honourable kills</span>
            </span>
            <span :style="{ display: 'flex', alignItems: 'center', gap: '5px' }">
              <span :style="{ width: '7px', height: '7px', background: DEATH, opacity: 0.55 }" />
              <span :style="{ fontSize: '12px', color: V.muted }">deaths</span>
            </span>
          </div>
        </UiPanel>
      </div>

      <UiPanel cap="Top killers" note="today · colour is class">
        <p
          v-if="!killers.length"
          :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
        >Nobody has scored an honourable kill yet today.</p>
        <div
          v-for="k in killers"
          :key="k.name"
          :style="{ display: 'grid', gridTemplateColumns: '96px 1fr auto auto', gap: '9px', alignItems: 'center', padding: '3.5px 0' }"
        >
          <button
            class="nm"
            :style="{
              appearance: 'none', background: 'none', border: 'none', padding: 0,
              cursor: 'pointer', fontFamily: FONT.body, fontSize: '13px', color: k.color,
              textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
            }"
            @click="emit('select', k.name)"
          >{{ k.name }}</button>
          <span :style="{ height: '7px', background: V.track, position: 'relative' }">
            <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: k.w, background: k.color }" />
          </span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.text, fontVariantNumeric: 'tabular-nums' }">{{ k.value }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint, minWidth: '74px', textAlign: 'right' }">{{ k.note }}</span>
        </div>
      </UiPanel>
    </div>
  </section>
</template>

<style scoped>
.nm:hover { text-decoration: underline }
</style>
