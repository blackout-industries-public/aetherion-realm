<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, fmt, spell, titled } from '../theme'
import { CLASS_COLOR, zoneName } from '../data'
import UiPanel from './UiPanel.vue'
import UiBars from './UiBars.vue'
import UiSpark from './UiSpark.vue'

const props = defineProps<{
  guilds: any | null; combat: any | null; llm: any | null; society: any | null
}>()

const CLASS_NAME: Record<number, string> = {
  1: 'Warrior', 2: 'Paladin', 3: 'Hunter', 4: 'Rogue', 5: 'Priest',
  6: 'Death Knight', 7: 'Shaman', 8: 'Mage', 9: 'Warlock', 11: 'Druid',
}

const mortality = computed(() =>
  (props.society?.mortality ?? []).map((m: any) => ({
    label: m.hour, value: m.deaths, second: m.revives,
  })))

// Deaths normalised by exposure. The population is exactly 250 per class, so the raw
// rate is already a fair comparison - no further weighting needed.
const whoDies = computed(() =>
  (props.society?.whoDies ?? [])
    .filter((w: any) => w.deaths > 0)
    .map((w: any) => ({
      label: CLASS_NAME[w.cls] ?? `Class ${w.cls}`,
      value: w.deaths,
      note: `${w.perBot.toFixed(2)}/bot`,
      tone: CLASS_COLOR[w.cls],
    })))

const ladder = computed(() =>
  (props.society?.ladder ?? []).map((l: any) => ({
    label: l.band, value: l.chars,
    note: l.dings ? `+${l.dings}` : `${l.avgGold}g`,
  })))

const loot = computed(() =>
  (props.society?.loot ?? []).map((l: any) => ({ label: l.hour, value: l.total })))

const lootMix = computed(() => {
  const rows = props.society?.loot ?? []
  const sum = (k: string) => rows.reduce((n: number, r: any) => n + (r[k] || 0), 0)
  return [
    { label: 'uncommon', value: sum('uncommon'), tone: 'oklch(0.74 0.10 158)' },
    { label: 'rare', value: sum('rare'), tone: 'oklch(0.70 0.13 250)' },
    { label: 'epic', value: sum('epic'), tone: 'oklch(0.68 0.15 305)' },
  ].filter(r => r.value > 0)
})

const balance = computed(() =>
  (props.society?.balance ?? []).map((b: any) => ({
    label: b.race, value: b.chars, note: `lvl ${b.avgLevel}`,
    tone: b.faction === 'alliance' ? 'oklch(0.68 0.13 250)' : 'oklch(0.62 0.17 25)',
  })))
const emit = defineEmits<{ select: [string] }>()

const ARCH_COLOR: Record<string, string> = {
  normal: 'oklch(0.74 0.085 158)',
  pro: T.gold,
  clueless: 'oklch(0.78 0.12 45)',
  afk: 'oklch(0.56 0.028 68)',
  scumbag: 'oklch(0.68 0.10 302)',
  roleplayer: 'oklch(0.82 0.08 350)',
  goldseller: T.red,
}

const topGuilds = computed(() => (props.guilds?.guilds ?? []).slice(0, 9))
const maxAvg = computed(() => Math.max(80, ...topGuilds.value.map((g: any) => g.avgLevel ?? 0)))

const lede = computed(() => {
  const n = props.guilds?.totalGuilds ?? 0
  const bosses = props.guilds?.bossesBeaten ?? 0
  if (!n) return 'No guilds have formed yet.'
  return `${titled(spell(n))} guild${n === 1 ? '' : 's'} ${n === 1 ? 'has' : 'have'} formed without ` +
    `anyone asking them to. ${bosses ? `${titled(spell(bosses))} boss${bosses === 1 ? '' : 'es'} ${bosses === 1 ? 'has' : 'have'} died to them.` : 'No boss has died to them yet.'}`
})

const hookOn = computed(() => !!props.llm?.hook?.enabled)
const bridgeUp = computed(() => !!props.llm?.bridge?.up)
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

    <div :style="{ display: 'grid', gridTemplateColumns: 'minmax(300px, 1fr) minmax(340px, 1.15fr)', gap: '16px', alignItems: 'start' }">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px', minWidth: 0 }">
        <UiPanel cap="Guilds" :note="`${fmt.int(guilds?.totalGuilds ?? 0)} formed · avg lvl`">
          <div
            v-for="g in topGuilds"
            :key="g.id"
            :style="{ display: 'grid', gridTemplateColumns: '18px minmax(90px, 1fr) 1fr 30px', gap: '10px', alignItems: 'center', padding: '5px 0' }"
          >
            <span
              :style="{
                width: '18px', height: '18px', border: `1px solid ${T.line}`,
                display: 'grid', placeItems: 'center', fontFamily: FONT.display,
                fontSize: '9.5px', color: T.muted,
              }"
            >{{ g.name?.charAt(0) ?? '?' }}</span>
            <span :style="{ fontSize: '14px', color: T.text, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ g.name }}</span>
            <span :style="{ height: '4px', background: T.lineSoft, position: 'relative' }">
              <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: `${((g.avgLevel ?? 0) / maxAvg) * 100}%`, background: T.goldDim }" />
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: T.muted, textAlign: 'right' }">{{ g.avgLevel ?? 0 }}</span>
          </div>
        </UiPanel>

        <UiPanel
          v-if="guilds?.beaten?.length"
          cap="Bosses beaten"
          :note="`${guilds.beaten.length} named`"
        >
          <div
            v-for="(b, i) in guilds.beaten"
            :key="b.boss"
            :style="{ display: 'flex', justifyContent: 'space-between', gap: '10px', alignItems: 'baseline', padding: '4px 0', borderBottom: i < guilds.beaten.length - 1 ? `1px solid ${T.lineFaint}` : 'none' }"
          >
            <span :style="{ minWidth: 0 }">
              <span :style="{ fontSize: '14px', color: T.text, display: 'block', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ b.boss }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">{{ b.instance }}</span>
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: T.green, whiteSpace: 'nowrap' }">×{{ b.kills }}</span>
          </div>
        </UiPanel>

        <UiPanel
          v-else-if="guilds?.kills?.length"
          cap="Recent boss kills"
          :note="`${fmt.int(guilds?.bossesBeaten ?? 0)} beaten`"
        >
          <div
            v-for="(k, i) in guilds.kills.slice(0, 8)"
            :key="i"
            :style="{ display: 'flex', justifyContent: 'space-between', gap: '10px', padding: '5px 0', borderBottom: i < 7 ? `1px solid ${T.lineFaint}` : 'none' }"
          >
            <span :style="{ minWidth: 0 }">
              <span :style="{ fontSize: '14px', color: T.text, display: 'block', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ k.boss }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">{{ k.instance }}</span>
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: T.muted, whiteSpace: 'nowrap' }">{{ k.players }}p</span>
          </div>
        </UiPanel>

        <UiPanel v-if="ladder.length" cap="The ladder" note="online, by level band">
          <UiBars :rows="ladder" label-width="72px" />
          <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.4 }">
            Right column shows level-ups in the last 24h where any happened, otherwise
            average gold held.
          </p>
        </UiPanel>

        <UiPanel v-if="loot.length" cap="Loot stream" note="6h, uncommon and above">
          <UiSpark :points="loot" hue="oklch(0.74 0.13 88)" label="drops per hour" />
          <div :style="{ marginTop: '10px' }">
            <UiBars :rows="lootMix" label-width="80px" />
          </div>
        </UiPanel>

        <UiPanel v-if="balance.length" cap="Faction and race" note="online">
          <UiBars :rows="balance" label-width="80px" />
        </UiPanel>

        <UiPanel cap="Open-world conflict" :note="`${fmt.int(combat?.killsToday ?? 0)} today`">
          <button
            v-for="f in (combat?.killers ?? []).slice(0, 8)"
            :key="f.guid"
            :style="{
              width: '100%', appearance: 'none', background: 'none', border: 'none',
              padding: '4px 0', cursor: 'pointer', display: 'flex',
              justifyContent: 'space-between', gap: '10px', alignItems: 'baseline', textAlign: 'left',
            }"
            @click="emit('select', f.name)"
          >
            <span :style="{ fontSize: '14px', color: CLASS_COLOR[f.cls] ?? T.text }">{{ f.name }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint, flex: 1, textAlign: 'right' }">{{ zoneName(f.zone).toLowerCase() }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: T.text, minWidth: '32px', textAlign: 'right' }">{{ f.kills }}</span>
          </button>
        </UiPanel>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px', minWidth: 0 }">
        <!-- The hook and the bridge fail independently. Saying which one is down is the
             difference between a useful panel and a red light. -->
        <section
          :style="{
            border: `1px solid ${hookOn && bridgeUp ? T.line : T.red}`,
            background: hookOn && bridgeUp ? T.panel : 'oklch(0.21 0.045 25)',
            boxShadow: T.inset, padding: '11px 13px',
          }"
        >
          <div
            :style="{
              fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
              letterSpacing: '.16em', color: hookOn && bridgeUp ? T.dim : T.red,
              textTransform: 'uppercase',
            }"
          >
            In-game LLM hook · {{ hookOn ? 'enabled' : 'disabled' }}
          </div>
          <p :style="{ margin: '7px 0 0', fontSize: '13.5px', color: T.textMid, lineHeight: 1.45 }">
            <template v-if="!bridgeUp">
              The bridge is not answering. Bots fall back to the canned reflex table until it returns.
            </template>
            <template v-else-if="!hookOn">
              The bridge is healthy and the model responds, but the worldserver is not calling it.
              Set <code :style="{ fontFamily: FONT.mono }">LLM_ENABLED=1</code> and restart the worldserver.
            </template>
            <template v-else>
              Live on {{ llm?.bridge?.model || 'the configured model' }}.
              <template v-if="llm?.hook?.requiresWitness">
                Calls only happen with a real player within {{ llm?.hook?.sayRange }} yards — an empty
                realm costs nothing.
              </template>
            </template>
          </p>

          <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: '10px', marginTop: '11px' }">
            <div v-for="s in [
              { l: 'p50 latency', v: llm?.bridge?.p50 != null ? `${llm.bridge.p50.toFixed(2)}s` : '—' },
              { l: 'served', v: fmt.int(llm?.bridge?.served ?? 0) },
              { l: 'max in flight', v: llm?.hook?.maxInFlight ?? '—' },
              { l: 'reasoning', v: llm?.bridge?.reasoning ?? '—' },
            ]" :key="s.l">
              <div :style="{ fontFamily: FONT.mono, fontSize: '15px', color: T.textHi }">{{ s.v }}</div>
              <div :style="{ fontSize: '11px', color: T.faint }">{{ s.l }}</div>
            </div>
          </div>
        </section>

        <UiPanel cap="Chatter" :note="`${fmt.int(llm?.chatter?.length ?? 0)} recent turns`">
          <p
            v-if="!llm?.chatter?.length"
            :style="{ margin: 0, fontSize: '13.5px', color: T.muted, lineHeight: 1.45 }"
          >Nothing said yet. Bots only reach the model when a real player is present.</p>

          <button
            v-for="c in (llm?.chatter ?? []).slice(0, 10)"
            :key="c.id"
            :style="{
              width: '100%', appearance: 'none', background: 'none', border: 'none',
              padding: '4px 0', cursor: c.bot ? 'pointer' : 'default', textAlign: 'left',
              display: 'grid', gridTemplateColumns: 'minmax(70px, auto) 1fr', gap: '9px', alignItems: 'baseline',
            }"
            @click="c.bot && emit('select', c.who)"
          >
            <span :style="{ fontSize: '13.5px', color: c.bot ? (CLASS_COLOR[c.cls] ?? T.gold) : T.muted, overflow: 'hidden', textOverflow: 'ellipsis' }">{{ c.who }}</span>
            <span :style="{ fontSize: '13.5px', color: T.textMid, lineHeight: 1.4 }">{{ c.text }}</span>
          </button>
        </UiPanel>

        <UiPanel v-if="mortality.length" cap="Mortality" note="deaths vs revives, 24h">
          <UiSpark
            :points="mortality"
            hue="oklch(0.66 0.19 25)"
            second-hue="oklch(0.62 0.10 158)"
            label="deaths"
            second-label="revives"
          />
          <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.4 }">
            The only recorded evidence of combat: this schema has no combat log.
          </p>
        </UiPanel>

        <UiPanel v-if="whoDies.length" cap="Who dies" note="level 55+, last 24h">
          <UiBars :rows="whoDies" label-width="96px" />
          <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.4 }">
            Exactly 250 characters exist per class, so these counts compare directly.
          </p>
        </UiPanel>

        <UiPanel cap="Cognitive archetypes" note="seeded from guid">
          <div :style="{ display: 'flex', height: '10px', gap: '1px', marginBottom: '9px' }">
            <span
              v-for="a in llm?.archetypes ?? []"
              :key="a.key"
              :style="{ width: `${a.pct}%`, background: ARCH_COLOR[a.key] ?? T.dim }"
              :title="`${a.label} ${a.pct}%`"
            />
          </div>
          <div :style="{ display: 'flex', flexWrap: 'wrap', gap: '10px 16px' }">
            <span v-for="a in llm?.archetypes ?? []" :key="a.key" :style="{ display: 'flex', alignItems: 'center', gap: '6px' }">
              <span :style="{ width: '7px', height: '7px', background: ARCH_COLOR[a.key] ?? T.dim, flex: 'none' }" />
              <span :style="{ fontSize: '13px', color: T.body }">{{ a.label }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">{{ a.pct }}%</span>
            </span>
          </div>
          <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.4 }">
            Derived from each character's guid, so a bot keeps its temperament across restarts.
          </p>
        </UiPanel>
      </div>
    </div>
  </section>
</template>
