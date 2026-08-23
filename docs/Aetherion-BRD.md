# Aetherion — Business Requirements & Design

Private World of Warcraft: Wrath of the Lich King (3.3.5a) realm with a simulated
population and an LLM social layer.

**Status:** Phase 1 and Phase 2 delivered and in service. One acceptance check (A12) awaits a
player logging in; see §10.1. Last full review and batched remediation: 2026-08-21
(§5.9, §5A, R4.7–R4.8, R7.5).
**Document purpose:** design authority for this system. Where this document and memory
disagree, this document wins; where this document and the running realm disagree, the
realm wins and this document is wrong and should be corrected.

---

## 1. Scope

| In scope | Out of scope |
| --- | --- |
| Single-realm 3.3.5a server, LAN-reachable | Public internet exposure |
| Simulated population (2500 bots) | Real concurrent player load beyond a handful |
| LLM-driven bot chat and social behaviour | LLM-driven combat rotations |
| Web dashboard for observing the world | Player-facing web features (armory, auction UI) |
| Reproducible build, backup, restore | High availability, multi-realm, clustering |

### 1.1 Deployment target

| Property | Value |
| --- | --- |
| Host | `nlucansk@10.10.25.193`, bare-metal Ubuntu, key-based SSH, passwordless sudo |
| CPU / RAM | 8 cores / 24 GB (22 GB usable) |
| Realm name / address | `Aetherion` / `10.10.25.193` |
| Client build | 3.3.5a (12340) |

**R1.1** Everything that can run in a container must run in a container, orchestrated by
Docker Compose. This overrides the upstream project's preference for native builds.

**R1.2** The host starts bare. All tooling is installed by the repository's own scripts;
no manual host preparation is a prerequisite.

**R1.3** The full stack must survive an unplanned reboot with no human intervention.
*Verified: confirmed after an operator reboot on 2026-08-20.*

---

## 2. System context

```
WoW 3.3.5a client ──▶ ac-authserver ──▶ ac-database (mysql:8.4)
                              │                 ▲
                              ▼                 │
                       ac-worldserver ──────────┘
                         │        │
              LlmBridge  │        │  MySQL reads
                         ▼        ▼
                     ai-bridge   warcraft-map (Nuxt)
                         │
                         ▼
              Bifrost gateway ──▶ LM Studio (Mac) ──▶ gpt-oss-20b (MLX)
```

| Container | Image | Role |
| --- | --- | --- |
| `ac-authserver` | `acore/ac-wotlk-authserver:playerbot` | SRP6 login, realm list |
| `ac-worldserver` | `acore/ac-wotlk-worldserver:playerbot` | World simulation, bots, our C++ patches |
| `ac-database` | `mysql:8.4` | `acore_auth`, `acore_world`, `acore_characters`, `acore_playerbots`, `aetherion_ai` |
| `ai-bridge` | `ai-bridge-ai-bridge` | FastAPI LLM mediator, port 8090 (loopback only) |
| `warcraft-map` | `frontend-warcraft-map` | Nuxt dashboard, port 3000 |

**R2.1** The database and SOAP ports bind to loopback only. Only the auth and world
ports are reachable from the LAN.

---

## 3. Upstream dependencies

All three are pinned to explicit commits. Tracking a moving branch in production is
prohibited — an upstream force-push would otherwise silently change the realm.

| Component | Repository | Commit |
| --- | --- | --- |
| AzerothCore (Playerbot fork) | `mod-playerbots/azerothcore-wotlk` @ `Playerbot` | `efe123fab543c5faf3c477674ec17a18fd59f09f` |
| mod-playerbots | `mod-playerbots/mod-playerbots` | `8d9f6aa6bc6d45f9ae0ee0675b9b1f8aa6937312` |
| mod-ah-bot | `azerothcore/mod-ah-bot` | `a680cc1c98290713e9b3d3289544af78e5186dc1` |

**R3.1** Our C++ changes live in `patches/llm/` and are applied by `patches/llm/apply.sh`,
never committed into the vendored module tree. `bootstrap.sh` re-applies them after every
re-pin, because pinning performs a detached checkout that would otherwise revert them.

**R3.2** `apply.sh` must be idempotent and must assert every anchor it edits. A patch step
that silently no-ops is treated as a defect — this rule exists because several early patch
steps reported success while changing nothing.

**R3.3** `patches/llm/` modifies mod-playerbots and therefore inherits **AGPL v3**. Source
must be offered to anyone the server is offered to.

---

## 4. Phase 1 — the realm

**R4.1** The realm hosts a simulated population of **2500 bots**, distributed across level
brackets rather than clustered at the cap. *Verified: 2453–2500 online; 7 GB of 22 GB used.*

**R4.2** Bots log in automatically and persist across restarts.

**R4.3** An auction-house bot maintains a populated economy.

**R4.4** Bots must not list bind-on-pickup items. Quest items and Darkmoon cards are
legitimately sellable and must remain so.

**R4.5** `PlayerSaveInterval = 60000`. This is a **product requirement, not a tuning knob**:
the dashboard reads positions from the `characters` table, so the save interval sets the
observable refresh rate of the map. At the shipped default the world appeared frozen.

**R4.6** Bots fight each other in the open world. *Verified: 420 world-PvP kills recorded.*
Requires `AiPlayerbot.Pvp.Enabled = 1`, which un-gates the `pvp` strategy that upstream
ships commented out.

**R4.7** Backups run on a schedule, prune old generations, and are **restore-verified** —
a backup that has never been restored is not a backup. *Corrected 2026-08-21: the 7/4/3
retention was half-implemented (weekly/monthly never promoted) and verification existed
only as a manual script. `backup.sh` now promotes Sundays and month-firsts, and
`warcraft-verify.timer` runs `verify-restore.sh` weekly; the chain was validated live.*

**R4.8** Bots fight battlegrounds. `RandomBotJoinBG` alone only grants the strategy;
**`RandomBotAutoJoinBG = 1` is what queues anyone.** Concurrency per bracket via
`RandomBotAutoJoinBG*Count` (WS 2 · AB 2 · AV 1 · EY 1 · IC 1). Per-match records
additionally need `Battleground.StoreStatistics.Enable = 1` in worldserver.conf.
*Verified: five battlegrounds running near-full, faction-balanced, ~40k BG kills/day;
first completed matches recorded in `pvpstats_battlegrounds` (7 matches, 284 player
rows) within 40 minutes of the key going live.*

---

## 5. Party, dungeon and raid subsystem

This is the largest piece of custom work. It lives in
`patches/llm/src/Ai/Party/PartyAssembler.{h,cpp}`.

### 5.1 Why it exists

Upstream forms groups only from bots that can currently *see each other*. Across four
continents that reliably produces two-bot parties and nothing larger, and a two-bot party
can never run a dungeon — so the dungeon finder never receives anything usable.

**R5.1** The assembler builds complete, role-plausible parties from the whole candidate
pool: one leader, then level- and faction-compatible members on the same map, filling a
tank and a healer before damage.

### 5.2 The travel model

**R5.2** A party travels to a dungeon the way a player would, choosing by distance:

| Distance | Method | Applies to |
| --- | --- | --- |
| Under `FootRange` (1200 yd) | Walk, using flight paths | Leader |
| Beyond, mage in group, 50% | **Portal** to nearest capital | Whole group |
| Beyond, otherwise | **Hearthstone** to nearest capital | Leader only |
| On arrival at the door | **Summon** at the meeting stone | Stragglers |
| No progress for `StallTicks` | Teleport the remainder | Leader |

Capitals are faction-aware per continent: Stormwind / Undercity (Eastern Kingdoms),
Darnassus / Orgrimmar (Kalimdor), Shattrath (Outland), Dalaran (Northrend).

**R5.3 — the stall fallback.** Grouped bots barely self-navigate. The original diagnosis
blamed `ProcessBot`'s early return for grouped bots; source verification later corrected
this — `PlayerbotAI::UpdateAI` runs for every bot regardless of grouping, and the real
mechanism is `AiFactory`, which grants the RPG movement strategies only to ungrouped bots
and group leaders (and the leader's own drive is weak). Measured either way: leaders sat
motionless at the exact hearth coordinates for 225 seconds, including one only 212 yards
from its destination.

The system therefore measures real progress each tick and, after `StallTicks` (2 ticks ≈
90 s) without closing at least 20 yards, teleports the leader the remaining distance and
reports it in-world as having taken the flight path.

This is consistent with upstream's own policy, not a departure from it: `MoveFarTo` already
teleports any bot that stops making progress toward its destination.

**R5.4** Dungeon choice is **nearest-first** — sorted by distance, then a random pick among
the closest `NearestChoices` (4). Choosing uniformly across a continent produced walks of
up to 10,448 yards against a 30-minute trip budget, which never completed. Post-change
trips run 212–4,300 yards.

### 5.3 Entrances and arrival points

**R5.5** Both sides of a dungeon door come from a single joined query over `areatrigger` +
`areatrigger_teleport`, with the level floor from `dungeon_access_template`:

- outdoor trigger position → where the party travels to
- `target_position_*` → where the party is placed on zone-in
- `min_level` → the level gate, standing in for attunement

This yields **69 doors and 71 arrival points**. The previous implementation read
`lfg_dungeon_template`, which holds only 24 rows and **no raids at all** — so most parties
had nowhere to zone into even when they arrived.

### 5.4 Raids

**R5.6** A share of assemblies (`RaidPct`, 20%) build a 10-person raid instead of a party.

**R5.7** `Group::ConvertToRaid()` must be called **before members are added**. A party group
reports itself full at five, so converting afterwards silently leaves a five-man. The flag
persists via `CHAR_UPD_GROUP_TYPE`, so the dashboard sees it.

**R5.8** A raid is only committed to when a reachable raid actually exists for that leader —
correct level, raid map, entrance on the same continent. A ten-man cannot be converted back,
so one formed with nowhere to go would stand still permanently. *This was observed: a level
36 leader formed a raid for content that does not exist below level 50.*

Raid coverage: 19 maps from Zul'Gurub (50) through Icecrown Citadel (80).

### 5.5 Inside the instance

Arriving is not the same as playing. Three defects kept every party inert once through
the door, each found only by watching live characters rather than reading code:

**R5.10 — members must be bound to their leader.** Upstream binds a bot to its leader
inside `AcceptInvitationAction`: `SetMaster`, `ResetStrategies`,
`ChangeStrategy("+follow,...")`, `Reset`. `Group::AddMember` bypasses that flow
entirely, so members carried `follow` and `dps assist` strategies with nothing to follow
or assist. The assembler now performs the same sequence for every member it adds.

**R5.11 — the party must be walked to the content.** `AiPlayerbot.AggroDistance` is 22
yards. Measured at a stranded party's entry point, the nearest creature spawn was **84
yards away**, so nothing was ever in range. Instance creature spawns (5,060 across 80
maps) are loaded at startup, and each tick the leader is steered to the nearest pack
beyond `HuntRange`. Members follow, so steering one character moves the whole party.

**R5.12 — the party must be able to leave.** See §10.1.

*Verified:* before these changes, five members sat at an identical position for 4.5
minutes with no movement. After, they spread out, positions changed every sample, and
health dropped and recovered — they were fighting.

**R5.13 — steer at bosses, and only skip what you are standing on.** `instance_encounters`
joined to `creature` gives 492 boss positions across 69 maps. Steering at the nearest
*trash pack* only wandered the entry hall; steering at a boss walks the party through the
trash in between. `HuntRange` must stay small (8 yd): at 28 yd a party that had closed to
20 yd was redirected to a different, further boss and oscillated between the two. The
level 61-70 bracket sitting at exactly 20 yards from a boss was that bug in one number.

### 5.6 What bots can and cannot do inside

Measured over eight hours of uninterrupted uptime, 2500 bots:

| content tier | instances entered | at least one boss down | rate |
| --- | --- | --- | --- |
| 10-40 | 180 | 33 | **18%** |
| 41-60 | 39 | 5 | **13%** |
| 61-70 | 135 | 1 | **1%** |
| 71-80 | 61 | 2 | **3%** |

**Bots clear trash but cannot kill high-level bosses.** In the same hour the 71-80 bracket
produced 60 loot drops and 15 deaths, so those characters are demonstrably fighting and
killing things — they simply do not get bosses down. The plumbing defects in §5.5 are
fixed and are not the constraint; what remains is bot combat capability (gear and
rotation) against encounters with an order of magnitude more health and real mechanics.

This bounds what the subsystem should be expected to deliver: a populated world where
low-level dungeons visibly progress and high-level ones are attempted but rarely cleared.
Raising the high tiers is a bot-itemisation and rotation problem, not an assembler one.

### 5.7 Lifecycle

**R5.9** A party holds its instance for `InsideTicks` (20 ticks ≈ 15 min), then disbands and
returns its bots to the candidate pool. Without this the assembler fills its quota once and
never forms another party for the remainder of uptime.

### 5.8 Verified behaviour

Measured on the shipped build at the shipped caps (`MaxParties 80`, `PerTick 3`),
2453 bots online, sampled after a full `InsideTicks` cycle had elapsed:

| Stage | Count |
| --- | --- |
| Parties formed | 94 |
| Raids formed | 5 |
| Trips started | 90 |
| Stall recoveries (§R5.3) | 73 |
| Arrived at the door | 67 |
| Entered the instance | **61** |

Trip-to-entry conversion **68%**, against 0% before this work. Dashboard read
`115 groups · 74 in instances · 3 raids`, from `0 raid · 0 in instance` at the start.
Host memory 9 GB of 22 GB with 74 live instances.

Travel mode mix: hearthed 76, portalled 8, on foot 6 — consistent with §R5.2, since most
candidate dungeons sit beyond `FootRange` and only some groups contain a mage.

Four observations worth carrying forward:

- **Stall recoveries (73) exceed arrivals (67).** §R5.3 is the dominant mechanism, not a
  rare safety net: grouped leaders largely do not walk, so the fallback is load-bearing.
  Removing it returns the system to 0% conversion. This is the system's main design debt —
  see §10.2.
- **94 parties formed against a cap of 80 confirms §R5.9 recycling.** Without the disband
  the count would pin at 80 and never move.
- **Dashboard `total` (115) exceeds `MaxParties` (80) legitimately.** It counts every group
  on the realm, including upstream's own proximity grouping, not just assembler-owned ones.
- **Sample earlier in the same run read 53%.** Trips in flight depress the ratio, so any
  measurement taken before a full cycle under-reports.

---

### 5.9 Protection from the realm's own manager

**R5.14 — owned groups are protected, and optionally driven.** Two source-verified
defects: (a) upstream's `LeaveOrDisbandGroup` was reachable for grouped bots through a
sticky "random bot update" flag — set once while solo, never cleared — dismantling
assembler parties at roughly one member per 17 hours; (b) parking grouped bots also
denied dead members the revive cycle, so a wiped party stayed dead until the sweeper
found it. `ProcessBot` now stops before its destructive limbs (disband, re-gear,
ungroup-and-teleport) for any assembler-owned group, and `Party.DriveGroupedBots = 1`
additionally passes owned members through for the revive cycle. Ownership is answered
from a mutex-guarded mirror because the call arrives from map threads.
*Validated 2026-08-21: disband log line for owned groups fell from ~12/15 min expected
to zero after deploy.*

**R5.15 — idle bots hand in finished quests.** Quest hand-in only runs inside the four
active RPG statuses, so a bot that completed objectives and then idled kept the reward
forever — measured 247 accepts per census interval against 8 rewards, with ~2,600
COMPLETE quests banked realm-wide. The Idle wake-up now drains one finished quest into
`DoQuest` before rolling a new pastime, skipping the `lowPriorityQuest` set so it cannot
ping-pong with the turn-in timeout.
*Validated 2026-08-21: rewards per interval 8 → 22 within two census windows, with 93
recorder-confirmed completions in the first 15 minutes.*

## 5A. Population budget

Five schedulers draw from the same 2500 characters with no arbiter: the party
assembler, battleground auto-join, upstream proximity grouping, the dungeon finder,
and the RPG status machine. Every activity added to the realm is paid for in bots
taken from another — most visibly from questing, which is what free bots do.

**R5A.1** The budget below is the *declared* allocation. Anyone changing an activity
knob is changing this budget and should update it here.

| Pool | Share (measured 2026-08-21) | Governing knobs |
| --- | --- | --- |
| Grouped PvE (parties, raids, instances) | ~31% | `Party.MaxParties` (80), `Party.PerTick`, `Party.RaidPct` |
| Battlegrounds | ~11% | `RandomBotAutoJoinBG*Count` (WS 2 · AB 2 · AV 1 · EY 1 · IC 1) |
| Free (questing, wandering, world PvP) | ~58% | `BotActiveAlone` × SmartScale band |
| — of which actually active | see R5A.2 | |

**R5A.2 — the activity throttle is the master lever, and it is multiplicative.**
The effective duty cycle of free bots is
`BotActiveAlone × (1 − (maxDiff − floor)/(ceiling − floor))`, where `maxDiff` is the
**maximum** world-update time over the last 500 samples. At the shipped 50/200 band a
single 200 ms spike pinned the multiplier to zero for the whole window: measured
effective activity was **0–1% regardless of `BotActiveAlone`** — the knob everyone
reaches for first is dead until the band is widened or tick time drops. Current
setting: `BotActiveAlone 25`, band 100/300.

**R5A.3** Grouped members have *no quest path at all* — `AiFactory` grants the RPG
strategies only to ungrouped bots and leaders, and battleground entry strips them.
Parking a bot is therefore a full opportunity cost, not a discount. Measured: the
30–49 band, parked at 33.5%, completes fewer quests per bot (0.5) than the 1–29 band
(0.9) whose under-15 half the assembler cannot touch.

## 5B-pre. Economy program

The bot-driven economy (auction house, needs engine, gold sinks, profession
supply chains, guild treasury) has its own BRD:
[Aetherion-Economy-BRD.md](Aetherion-Economy-BRD.md) — 10 epics, ~60 stories,
adversarially reviewed 2026-08-21. It composes with race mode (its section 9)
and its E0-E5 are the recommended precondition for starting the race.

## 5B. Race mode — the wipe-to-endgame simulation

The realm can be restarted as a race: every character destroyed, every bot recreated
at level 1 with only its starting outfit, and the recorder arbitrating realm-firsts
(levels 10–80, boss kills, full clears) on the RACE tab. Built 2026-08-21; **armed
but never run without the data-loss confirmation in `scripts/wipe-race-start.sh`.**

**R5B.1 — why a plain wipe cannot work (source-verified).** Two active systems
overwrite naturally earned levels: (a) every bot below level 3 is routed to
`RandomizeFirst` on its periodic randomize — whose first firing is seconds after
login — re-rolling it to `urand(1, 80)`; the starting line would dissolve within a
minute. (b) `LevelBrackets` enforcement redistributes bots into a fixed 12/11/…%
pyramid every 300 s via a full factory reset (level, quests, skills, spells), and it
does **not** exempt assembler parties. Both were confirmed with line-level evidence
in `RandomPlayerbotMgr.cpp` and `RandomBotLevelMgr.cpp`.

**R5B.2 — the fix set.** `RACE_MODE=1` in `overlay/.env` (written by the wipe
script, honoured by `configure.sh` so C3 holds) flips `DisableRandomLevels = 1`
(pins the one-time init to level 1), `LevelBrackets.Enabled = 0`, and parks Death
Knights (`DisableDeathKnightLogin = 1` — they cannot start below 55 and would top
every board on day one). `patches/llm/patch_racemode.py` makes the periodic
randomize a **one-time init**: the factory runs once per fresh character, then never
again — no level knock-backs at level 1–2, no gold re-rolls, no gear churn. The
patch is gated on `DisableRandomLevels`, so the deployed binary is inert until the
config arms it.

**R5B.3 — "with nothing" and its deliberate concessions.** Starting wealth is
hardcoded upstream (10–50 g per level, four free bags, consumable stocking), so the
patch cuts those grants and `Refresh()`'s inventory wipe, free supplies and money
floor. Kept free on purpose: spells, skills, pets, mounts, ammo and reagents —
bots have no trainer/vendor behaviour, so removing these breaks classes rather than
purifying the economy. Treat them as "the bot trained and restocked off-screen".
The AH bot is disabled in race mode (`AHBOT_IN_RACE` overrides): a vendor-stocked
AH would hand the racers an economy they did not build, and after a wipe
`AHBOT_CHAR_GUID` points at whichever new character inherited the guid.
`BOT_XP_RATE` is the pace dial (1.0 = authentic; the climb takes weeks).

**R5B.4 — the wipe procedure** (`scripts/wipe-race-start.sh`, triple-gated: flag,
exact-target-name confirmation, verified fresh backup). Drops `acore_characters` +
`acore_playerbots`, truncates `acore_auth.realmcharacters` (stale per-account counts
feed the char-creation limit; nothing else in auth references characters), re-imports
schema via `ac-db-import`, truncates every `aetherion_ai` table, inserts the
`race_start` milestone that starts the RACE-tab clock, arms `RACE_MODE=1`, restarts
the worldserver (rndbot accounts are reused; characters recreate because the
per-account count is read live from `acore_characters`), and force-recreates the
recorder so its in-memory baselines start clean. Rollback is `scripts/restore.sh`
with the backup the script itself just verified.

### 6.1 Topology

**R6.1** Inference is routed through the **Bifrost gateway**, never directly to a model
host. Bifrost prefixes model IDs with the provider name, hence the `lm studio/` prefix.

| Property | Value |
| --- | --- |
| Base URL | `https://bifrost.cluster.blackout.industries/v1` |
| Model (interactive + background) | `lm studio/openai/gpt-oss-20b` (MLX) |
| `reasoning_effort` | `low` |
| Bridge | `ai-bridge`, port 8090, loopback only |

### 6.2 Model selection

**R6.2** A model qualifies only if it returns non-empty `content` and does not leak its
reasoning into chat. Measured head-to-head, six prompts each at `reasoning_effort: low`:

| | previous (GGUF) | **current (MLX)** |
| --- | --- | --- |
| Median latency | 1.25 s | **0.52 s** |
| Reasoning leaked into chat | 3 of 4 | **0 of 6** |
| Requests completed | 4 of 6 | **6 of 6** |

Leak rate matters more than latency: a leak trips the bridge's `_META` rejection and forces
a retry, so the previous model's true cost was roughly double its measured latency.
End-to-end through the bridge the current model serves at p50 **0.609 s**, mean 0.72 s.

### 6.3 Cost control

**R6.3** No LLM call is made for a conversation with **no real human present**. This
includes guild chat. Bot-to-bot chatter is served from a canned reflex table (19 patterns)
and a weighted topic model, not from the LLM.

**R6.4** A single responder is chosen per utterance (`TryClaim` / `PickResponder`) so one
player message does not fan out into a dozen inference calls.

*Verified:* with the hook enabled and 2410 bots live, bridge `served` held at 3 — the
verification probes and nothing else — across two samples seven minutes apart, with zero
connection failures logged. A fully populated realm with no human in it costs nothing.

### 6.4 Behaviour

**R6.5** Bots have differentiated cognitive archetypes — competent, average, careless,
absent, opportunistic — derived deterministically from character GUID so a given bot is
consistent across restarts.

**R6.6** Bots link items as real clickable item links, not plain names.

**R6.7** Bot intent is restricted to an allow-list; requests are gated and refusals guarded.
The LLM proposes, the C++ side disposes.

### 6.5 Persistence

`aetherion_ai` schema on the same MySQL instance:

| Table | Contents |
| --- | --- |
| `turns` | Conversation history per bot |
| `relationship` | Per-bot, per-speaker standing |
| `bot_events` | Activity feed backing the dashboard's per-bot history |

### 6.6 Bridge API

`GET /health` · `GET /metrics` · `GET /bot/{name}` · `GET /bot/{name}/history` ·
`POST /chat` · `POST /game/whisper` · `POST /game/event` · `POST /game/greet`

---

## 7. Dashboard

**R7.1** A Nuxt application renders the live world on the real Warcraft map, with no
authentication, containerised alongside the realm.

**R7.2** Map art is generated **from a client the operator owns**, by
`frontend/tools/extract_maps.py`. It is never downloaded or redistributed, and never
committed.

**R7.3** The dashboard is organised as **WORLD · PVE · PVP · SOCIETY · GUILDS · OPS**.
WORLD carries the map with switchable layers (activity, population hotspots, world-PvP,
professions — single-hue sequential ramps, binned cells, contrast measured per dot).
PVE: assembler board with named encounter progress, clear rates, dungeon demand, quest
progression. PVP: live battleground occupancy split by faction, honourable-kill split
(battleground vs world; NPCs never count — verified in `Player::RewardHonor`), tempo,
arena teams. SOCIETY: demographics, mortality, loot stream, LLM hook state, archetypes.
GUILDS: standings, per-guild pulse and detail. OPS: reachability, feed freshness, event
ingest, restart churn, DB footprint, census, acceptance, and the last SQL error.

**R7.4a — deep world.** Clicking a continent dives into the zone under the cursor:
65 per-zone maps extracted from the operator's client (`tools/extract-zone-maps.sh`),
with projection bounds read from the same archives' `WorldMapArea.dbc` so art and
coordinates cannot disagree. Zone membership is decided by position rectangle, not the
character's zone column, so bots plotted at a dungeon door appear on the zone the door
stands in. All map layers and trails follow into the zone. Dalaran city has no
standard tile set and is deliberately absent — presence in the generated table is the
capability check. Zone art files match `zone-<areaId>.jpg` under the same strict
route allowlist.

**R7.5** API queries fail *loudly but non-fatally*: the shared `q()` helper logs each
distinct SQL error once and exposes the latest on the Ops tab. This rule exists because
`.catch(() => [])` silently swallowed a collation error and an `ONLY_FULL_GROUP_BY`
error — a permanently failing query was indistinguishable from an empty result.

**R7.4** Runtime configuration must be read at runtime. Nuxt bakes `runtimeConfig` and the
public asset manifest at build time, so database settings arrive via `NUXT_*` environment
variables and map tiles are served by a runtime route rather than as static assets.

---

## 8. Configuration reference

All values are set by `scripts/configure.sh`, are environment-overridable, and are
re-applied idempotently after every upgrade.

### 8.1 Party assembler

| Key | Value | Meaning |
| --- | --- | --- |
| `Party.Enabled` | 1 | Master switch |
| `Party.IntervalMs` | 45000 | Tick period; all tick-based budgets derive from this |
| `Party.PerTick` | 3 | Assemblies attempted per tick |
| `Party.MaxParties` | 80 | Concurrent assembler-owned groups |
| `Party.TargetSize` / `RaidSize` | 5 / 10 | Party and raid size |
| `Party.RaidPct` | 20 | Share of assemblies that attempt a raid |
| `Party.MinLevel` / `LevelSpread` | 15 / 4 | Eligibility |
| `Party.FootRange` | 1200 | Walk below this, shortcut above |
| `Party.PortalPct` | 50 | Chance a mage portals rather than the leader hearthing |
| `Party.NearestChoices` | 4 | Random pick among the N closest dungeons |
| `Party.StallTicks` | 2 | Ticks without progress before the teleport fallback |
| `Party.ArriveRange` | 60 | Yards counted as "at the door" |
| `Party.MaxTripTicks` | 40 | Travel budget (~30 min) |
| `Party.InsideTicks` | 20 | Instance dwell before disband (~15 min) |

### 8.2 Grouping and PvP

`Grouper.SoloPct` 10 · `MemberPct` 50 · `Leader2Pct` 5 · `Leader3Pct` 5 · `Leader4Pct` 10
(remainder is LEADER_5; must total 100) · `Pvp.Enabled` 1

### 8.3 LLM

`Llm.Enabled` · `Host` `ai-bridge` · `Port` 8090 · `TimeoutMs` 15000 · `MaxInFlight` 8 ·
`RequireHumanWitness` 1 · `SayRange` 45 · `ChannelReplyChance` 25 · `EventsEnabled` 1

---

## 9. Constraints and invariants

**C1** Never commit game client data, DBC files, Blizzard map art, or any credential.
`.gitignore` excludes `frontend/maps/*.jpg`, `**/.env`, `azerothcore/`.

**C2** Values containing spaces in `.env` **must be quoted**. `MODEL_INTERACTIVE` contains
`lm studio/`; unquoted, it breaks every `source .env` in every script.

**C3** Any setting that must persist belongs in `overlay/.env`, not applied by hand.
`configure.sh` rewrites the generated config from defaults on every run, so a hand-edited
value is silently reverted — see §10.1 for a live instance of exactly this.

**C4** `overlay/.env` is the single source of truth; `azerothcore/.env` is a copy written by
`bootstrap.sh`. `ai-bridge/.env` is a symlink to the former.

**C5** Node uses pnpm only; Python uses uv only. Build and run inside Docker.

**C6** Irreversible data operations stop for explicit confirmation.

**C7** The repository `Blackout-Industries/aetherion-realm` is **private** by deliberate
choice.

---

## 10. Open items

### 10.1 In-game LLM hook — resolved

`LLM_ENABLED` was absent from `overlay/.env`, so `configure.sh` reset
`AiPlayerbot.Llm.Enabled` to `0` on each of several runs made while tuning the party
system. `LLM_ENABLED=1` is now in `overlay/.env` and therefore survives `configure.sh` (C3).

`.reload config` does **not** pick this flag up — the module caches it at startup — so a
worldserver restart was required. Done; the server now logs
`LLM bridge enabled -> ai-bridge:8090` on boot.

Chain verified end to end, except for the trigger:

| Link | Method | Result |
| --- | --- | --- |
| Flag loaded by running server | Startup log | `LLM bridge enabled -> ai-bridge:8090` |
| Worldserver resolves the bridge | `getent hosts` in-container | `172.19.0.2 ai-bridge` |
| Worldserver reaches port 8090 | HTTP GET over `/dev/tcp` in-container | `{"status":"ok","llm_backend":"up"}` |
| Bridge reaches the model | `POST /chat` with a live bot name | p50 0.609 s, `source: "llm"` |

**The remaining link needs a person.** Every LLM entry point — chat, greet, and event —
requires a real player in world by design (R6.3); `GreetWorker` and `EventWorker` both take
a human GUID. With nobody logged in, the bridge correctly serves zero traffic, and bridge
counters showing only the verification probes is the cost control working, not a fault.

To confirm: log in and `/say` something near a same-faction bot within `SayRange` (45 yd),
then check that `served` on `GET /metrics` has climbed.

### 10.2 Lower priority

| Item | Note |
| --- | --- |
| **Grouped bots do not self-navigate** | The teleport fallback still carries most trips. Source-verified 2026-08-21: the cause is `AiFactory` withholding RPG strategies from group members, **not** `ProcessBot` — a ProcessBot change cannot restore visible travel (see §5.9 for what it does fix). Making members walk means granting the member branch a movement strategy, which changes group combat behaviour and needs its own test cycle. |
| Boss names on the dashboard | Resolved 2026-08-21: the 612-row `dungeonencounter_dbc` seed was applied (`scripts/seed-encounters.sh`, upsert, no truncate). Boss kills now resolve to names on the PVE view and in the milestone recorder. |
| No combat log | Confirmed absent across every schema: no damage, casts, threat or trash kills. Deaths and loot are the only evidence a fight happened, which is what §5.6 reports. |
| **High-level bosses are not killed** | 1% clear rate at content 61-70 and 3% at 71-80, against 18% at 10-40 — while the same characters loot and die normally, so they are fighting. Needs bot gear and rotation work, not assembler work. See §5.6. |
| Bots hearing `/say` | The 4-argument `OnPlayerCanUseChat` override is not implemented |
| Achievement links | Implemented but not wired to a chat topic |
| Boss-kill progression | Instance entry is verified; kills populating `log_encounter` is not |
| Realm addressing | Realm holds a DHCP lease; Bifrost points at a Mac IP that moves |
| Homelab | `k8s-worker02` NotReady; Cilium operator API VIP `10.10.99.99` |
| Observability | BRD calls for Grafana/OTEL; dashboard currently covers this |

---

## 11. Acceptance criteria

| # | Criterion | Status |
| --- | --- | --- |
| A1 | Stack builds from a bare host via repository scripts | Met |
| A2 | Stack survives reboot unattended | Met — verified |
| A3 | 2500 bots online and distributed across brackets | Met |
| A4 | Backups run, prune, and restore successfully | Met — restore verified |
| A5 | Bots form parties larger than two | Met |
| A6 | Parties travel to and enter dungeons | Met — 7 entries observed |
| A7 | Raids form, convert, and enter | Met — Trial of the Crusader, 10 members |
| A8 | Bots fight in the open world | Met — 420 kills |
| A9 | Dashboard shows live world on real map art | Met |
| A10 | LLM replies are in-character with no reasoning leakage | Met at the bridge |
| A11 | LLM spend is zero when no human is present | Met by design |
| A12 | Bots converse in-game via the LLM | Hook enabled and wiring verified; final confirmation needs a player in world — §10.1 |
