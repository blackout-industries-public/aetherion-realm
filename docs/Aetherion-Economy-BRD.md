# Aetherion Economy — Business Requirements & Design

Companion to [Aetherion-BRD.md](Aetherion-BRD.md). Scope: a genuine, closed-loop,
bot-driven economy — bots sell what they gather and craft, buy what they need,
and need gold for real things. No artificial market-making: the auction house
starts empty and every listing, bid and coin in it comes from a bot's own
behavior. Drafted 2026-08-21 from five source probes against the deployed
module; revised same day after a three-lens adversarial review (coverage,
technical, trackability — 32 findings folded in, the significant ones marked
"rev:" inline). All file:line references verified on the realm host before it
went offline; re-verifiable against the identical local staging tree.

---

## 1. Vision

A herbalist picks flowers and sells them. A herbalist-alchemist brews potions
from her own flowers and sells those instead. A warrior who dinged 40 wants a
50g mount, checks his purse, and goes to the auction house with a bag of loot
because he *needs the gold*. A guild pools deposits until an officer can buy a
bank tab, and a broke member repairs on the guild's coin.

Two hard principles:

- **P1 — No artificial economy.** mod-ah-bot ("Marketeer") is retired. The AH
  starts empty. If nothing appears, that is a true reading of bot behavior and
  the bug is upstream of the market, not papered over inside it.
- **P2 — Packet-first.** Every economic mutation goes through the bot's own
  session as a real client opcode, so core validation, costs, SQL persistence
  and mail flow are inherited, never reimplemented. rev: the safety property is
  the call-site invariant, not per-opcode flags — `HandleBotPackets` executes
  whatever is queued and is safe because its sole caller runs on the world
  thread (PlayerbotMgr.cpp:258; see the risk table). Any deliberate exception
  to P2 must be named in its story (currently exactly one: E5.1's interim mail
  mechanism, if option (b) is chosen).

And one method principle, per operator instruction:

- **P3 — Small, observable increments.** Every story is one PR-sized change
  validated alone, and every acceptance criterion is a measurement against a
  snapshotted baseline — "we implement the need, then we watch whether bots
  develop the need."

## 2. Ground truth (what exists today)

| Area | Reality |
| --- | --- |
| Professions | Assignment real and persistent (`playerbots_random_bots`, 2500 rows). Gathering GENUINE — real casts, real herbs/ore in bags — but opportunistic within a ~150y scan of the wander path. Crafting NEVER runs (CraftRandomItemAction orphaned). Skill numbers synthetic (re-pinned to level*5 on every levelup AND every factory Refresh — two call sites). |
| Item fate | Goods hoarded, then periodically DESTROYED (`ClearInventory` in every factory Randomize; SmartDestroyItemAction destroys AH-usage and SKILL items under bag pressure). Nothing is ever sold — no loaded strategy triggers SellAction. |
| AH | All 741 auction rows ever belong to "Marketeer" (mod-ah-bot). Zero bids in realm history. Every AH handler requires a nearby auctioneer NPC — no remote listing exists under P2. |
| Banks | Deposit/withdraw code exists (BankAction) but is master-chat-only; 0 of 3002 characters have ever used a bank slot. Bankers are already wander targets. Latent mask bug: combined IterateItems masks silently skip the bank (InventoryAction.cpp:47). |
| Mail | Collection code exists (TakeMailProcessor) but is master-gated AND proximity-locked: every mail-take handler passes `CanOpenMailBox`, which hard-fails without an interactable mailbox GO (MailHandler.cpp:39-63). Uncollected mail items are deleted at mail expiry — an invisible destruction channel. |
| Gold | Faucets: quest money (~1.5-4g/hr mid-level) + corpse coin (~1-2g/hr); vendor trash (~3-8g/hr) trapped. Sinks: taxi only. Repairs are free at EIGHT call sites — dominated by the death-release pair (every death fully repairs for free). Training, mounts, gear, consumables all free via factory paths. |
| Needs machinery | BudgetValues: a working, 60s-cached money-needs oracle with almost no live consumers. One free-choice point in the new-RPG state machine (RPG_IDLE); our deployed quest-drain patch already proves the preemption pattern there. |
| Guilds | 60 preset guilds (created free, administratively), all saturated 25/25; BankMoney=0, 0 tabs, 0 petitions ever. Organic petition pipeline exists behind the disabled "guild" strategy — but the charter BUY step is wired only into the unloaded legacy rpg strategy, so "+guild" alone cannot produce petitions. Guild bank core support complete (tabs 100g-5000g); guild vaults are GameObjects, not creature bankers, and are not wander targets. |

## 3. Architecture

### 3.1 The needs engine (the spine)

A per-bot ledger of explicit needs, evaluated on a staggered per-minute pass,
exported to telemetry, and consulted at the RPG_IDLE choice point.

- **Taxonomy** — superset of `NeedMoneyFor`: `repair`, `training`, `ammo`,
  `consumables`, `travel` (rev: was missing — the one currently-real expense),
  `mount`, `gear_slot`, `materials`, `bank_space`, `guild`, `ah_capital`.
- **Evaluation** — reuses the 60s-cached budget values; hazards pre-solved as
  stories: a global trainer-cost index (E1.2 — rev: covering class AND
  profession trainers, since profession-rank training is a real need under
  E7.2), and a cached `WorstSlotsValue` (E1.3).
- **Behavior** — a config-gated preemption block in the IDLE branch (shipped
  empty as E2.0, statuses registered one by one). New statuses are added
  **append-only** to the NewRpgStatus enum (rev: inserting shifts every stored
  status int and mislabels the whole live map), and each new status ships its
  frontend mapping entry (WorldView hardcodes the known cases).
- **Telemetry** — `aetherion_needs` on the PartyAssembler pattern (create at
  startup, per-minute truncate + batched async inserts).

### 3.2 Money flow discipline

Every paid behavior gates on `free money for <need>` — never raw `GetMoney()`.
rev: the stub level^3 budget formulas are NOT a phantom cross-cutting task —
each consuming story's acceptance includes replacing its own formula with the
real price (E3.1 repair dry-run, E3.2 the E1.2 index, E3.3 vendor mount
prices, E4.1 ah_capital).

### 3.3 Sequencing constraint (from the income model)

Income first (E2), then sinks (E3), then market (E4+). rev: the solvency gate
this depends on is now BUILT, not assumed: E1.7 ships per-bot gold snapshots
and the income-rate panel, and at least a week of baseline is captured before
E2 flips on. Every phase gate re-runs the E0.0 baseline query set.

### 3.4 Patch engineering

rev: rewritten — the original "run after patch_racemode, anchor on
post-racemode text" rule was wrong for the spine's own file. Correct rules:

- File ownership: `NewRpgAction.cpp` (the IDLE branch) is owned by
  `patch_questturnin.py` (apply.sh step 2c). The needs-preemption patcher is
  FOLDED INTO or ordered directly after it, anchors on questturnin's
  replacement text, and `NewRpgAction.cpp` is added to apply.sh's restore-first
  git-checkout list. The racemode-ordering rule applies only to the files
  racemode owns (`RandomPlayerbotMgr.cpp`, `PlayerbotFactory.cpp`).
- Every patcher is marker-idempotent and tested against fresh AND
  already-patched trees (the racemode test procedure).
- Every behavior has an `AiPlayerbot.Econ.*` kill-switch, default off, armed
  via configure.sh (`ECON_*` env per C3). Rollback is a config flip — and the
  flip-OFF path is DRILLED per epic (see DoD), not assumed.

### 3.5 Race-mode interplay

Race gates remove unearned wealth; the economy provides earned wealth and real
expenses. E3.7 transfers each remaining freebie to a paid behavior as the
matching story lands. Economy pricing validates against race-mode income
(2.5-6g/hr mid-level), not legacy factory-endowed gold.

### 3.6 Coexistence with the deployed machinery (rev: new)

- **PartyAssembler drafting**: a bot mid-economy-errand can be drafted into a
  party (Candidates() does not filter on RPG status), teleported, and its
  status clobbered. Decision: accept clobber — needs re-preempt at the next
  IDLE; the assembler sweep additionally skips bots in GO_AH/GO_MAIL (mid-
  transaction states that must not teleport away from the auctioneer).
- **Activity throttle**: unwatched bots run at a low duty cycle; economy
  travel legs inherit that cadence. Economy GO_* statuses get the same
  AllowActive exemption as assembler trips (patch_tripactive pattern), config-
  gated, or acceptance windows widen — decided per status by measured travel
  completion rates on staging.

## 4. Epics

Sized S/M/L. **Standard definition of done for every epic** (rev): epic KPI
green on staging, then production, for a stated soak period; all of the epic's
event types emitting into `aetherion_econ_events`; rollback drill passed (flip
the epic's Econ.* switch off on staging, verify the pre-epic baseline
measurement returns). Stories that are one-way (data, not config) say so.

### E0 — Foundations (S)

| Story | What | Acceptance |
| --- | --- | --- |
| E0.0 | rev: BASELINE SNAPSHOT, first story of the program, before anything changes: archive a fixed dated query set — auction count, petition count, bankSlots distribution, guild BankMoney + guild_bank_item, trade-goods held by subclass, mail count, per-level-band gold supply, character_skills vs level*5 conformance, worldserver mean/p99 update diff | archived file in docs/baselines/; the same set re-runs at every phase gate |
| E0.1 | Retire mod-ah-bot; AH drains to empty naturally | auctionhouse rows -> 0 within 48h (E0.0 preserved the evidence) |
| E0.2 | `AiPlayerbot.Econ.*` namespace + `ECON_*` configure.sh gates, default off | plain configure run changes nothing live |
| E0.3 | Patch-chain policy per 3.4 (restore-first list extension, ownership map) | apply.sh idempotent on fresh and patched trees |
| E0.4 | Staging discipline: every epic validates on the MacBook instance first | staging realm runs the epic build + KPI query before production |

### E1 — Needs engine + instrumentation, observe-only (M)

Zero behavior change; bots develop needs, the operator watches. rev: this epic
now also owns the instrumentation that later acceptance criteria consume.

| Story | What | Acceptance |
| --- | --- | --- |
| E1.1 | Needs taxonomy + ledger structs | staging assertion: a bot forced to 60% durability yields a repair need within one pass, amount equal to the DurabilityRepair dry-run cost |
| E1.2 | Global trainer-cost index, class AND profession trainers, startup-built | "train cost" readable fleet-wide with no template scans; tick diff unchanged vs E0.0 snapshot |
| E1.3 | `WorstSlotsValue` — cached per-slot gear scores | worst-3 slots per bot; recompute <1ms/bot |
| E1.4 | Staggered needs pass (1/10th fleet per tick) | full fleet per minute; tick diff delta <5ms vs the E0.0 measurement source |
| E1.5 | `aetherion_needs` export + NeedsStatistic | seeded control: one staging bot forced into each need state, row appears within 60s |
| E1.6 | Frontend NEEDS panel (funded vs starved) | panel counts equal the SQL cross-check within one polling interval |
| E1.7 | rev: `aetherion_gold` per-bot snapshot (guid, level, money, ts) + income-rate g/hr by level band panel — the solvency gate's data source | >=7 days of baseline captured before E2 enables; panel matches E0.0 gold query |
| E1.8 | rev: `aetherion_econ_events` schema + OBSERVE-ONLY emitters for what already happens: item destruction (SmartDestroy, ClearInventory), mail expiry with content | destruction baseline measured — the number E2.3 is judged against |

### E2 — Income: unlock the vendor faucet (M) — MUST precede E3

| Story | What | Acceptance |
| --- | --- | --- |
| E2.0 | rev: the needs-preemption framework block in the IDLE branch, config-gated, ZERO statuses registered — every later GO_* story depends on this | Econ off and on behave identically (nothing to preempt to); tick diff unchanged |
| E2.1a | GO_VENDOR, nearby case: vendor/repair-flag filter over the existing 150y scan + arrival within interaction range | bots at vendors in a new map status (frontend mapping entry included) |
| E2.1b | rev: far-vendor resolver (spawn-data query, GoCamp-style travel) — the common full-bags-in-the-field case | vendor-trip completion rate from >150y measured on staging |
| E2.2 | Sell on arrival via a NEW SellAction mode: greys + ITEM_USAGE_VENDOR, EXCLUDING ITEM_USAGE_AH (rev: the existing "vendor" visitor sells AH goods too — the needed filter did not exist); ShouldSell guard so AH-hold-heavy bags do not re-trigger unclearable trips | vendor-sale events flow (emitter from E1.8 pattern); income-rate panel (E1.7) rises against its baseline |
| E2.3 | Bag triage: keep/vendor decisions + never destroy class=7 trade goods. rev: hold-flags are inert markers with a bag-pressure ceiling — above it, cheapest AH-hold goods are vendored as interim overflow relief until E4 ships (config-gated, removed at E4) | destruction events for trade goods -> 0 vs the E1.8 baseline; bag-fullness telemetry stays bounded |
| E2.4 | ClearInventory trade-goods exemption. rev: scoped as defense-in-depth for non-race operation (race mode already one-times Randomize); acceptance restated | with race gates OFF on staging, a forced randomize preserves trade goods |
| E2.5 | rev: SELL WHEN BROKE — the operator's central ask, previously unimplemented: an unmet, funded-below-threshold need plus sellable inventory triggers GO_VENDOR regardless of bag pressure | sell events correlate with low `free money for <need>` in aetherion_needs, not only with bag fill |

### E3 — Sinks: needs get teeth (L)

Free path stays behind a config fallback per story; enable only when E1.7
shows solvency.

| Story | What | Acceptance |
| --- | --- | --- |
| E3.1 | Paid repairs. rev: gate ALL EIGHT free DurabilityRepairAll sites (ReleaseSpiritAction x2 — the dominant path, every death repaired free; TrainerAction x2; UseMeetingStone; factory Refresh, AutoGear; mgr Refresh), with an explicit death-path decision: remove release-time repair so death costs durability per vanilla rules | free-repair invocations by random bots -> 0 (counter on gated sites) AND repair-gold-spent > 0; no perma-broken bots (funded-need telemetry) |
| E3.2 | Paid class training: gate the free InitClassSpells path; GO_TRAIN status, TrainerAction + E1.2 index; budget formula replaced by the index | per-level training spend lands within the modeled per-level band; no spell-less bots (audit) |
| E3.3 | Paid riding + mounts: gate InitSkills riding + InitMounts; buy riding + mount when funded; mount budget from vendor prices | first BOUGHT 50g mount is a feed event; mount-need funnel visible end to end |
| E3.4a | rev (split): drop `food` from BotCheats | food purchases appear; bot survivability unchanged (death-rate telemetry) |
| E3.4b | rev (split): un-exempt InitAmmo from the race gates | hunters buy ammo with earned gold; hunter DPS audit clean |
| E3.4c | rev (split): vendor restock buying via BuyAction budgets on GO_VENDOR trips | restock purchases flow; consumables budget formula replaced |
| E3.5 | (pointer) bank slot purchases — story lives in E6.4 | — |
| E3.6 | Optional: paid respec (rank below the others) | escalating respec costs appear when enabled |
| E3.7 | Retire the trainer-behaviour exemption: each racemode-kept freebie moves to its paid path as the matching story lands | racemode docstring updated per story; no freebie without a race gate or a paid replacement |

### E4 — Auction house: sell side (M)

| Story | What | Acceptance |
| --- | --- | --- |
| E4.1a | rev (split+reorder): minimal AhSellAction — fixed-price CMSG_AUCTION_SELL_ITEM, PLUS the opportunistic arrival hook (auctioneers are already wander targets), so listings appear from natural wandering and the story is observable alone | listings owned by ordinary bots exist; deposits charged |
| E4.1b | Pricing: vendor-anchored + underprice roll; ah_capital budget formula | price distribution sane vs vendor value (query) |
| E4.1c | Listing caps + duration policy (12/24/48h) | caps respected; no bot floods the house |
| E4.2 | Deliberate GO_AH travel (need-driven, incl. E2.5's sell-when-broke routing to AH for trade goods) | AH visit rate rises vs the E4.1a opportunistic baseline |
| E4.3 | Profession-aware listing: surplus goods the bot's own professions do not consume get listed; AH-hold branch of E2.3 feeds this | listed mix correlates with seller professions |
| E4.4 | OnAuctionAdd/Successful/Expire hooks -> econ events + market board panel | every market event visible within one poll |

### E5 — Mail leg (S then S) — rev: hard edge E4.1a -> E5.1 (auction
proceeds are the only mail bots receive; nothing to collect before sales)

| Story | What | Acceptance |
| --- | --- | --- |
| E5.1 | M0 interim collection. rev: the packet path is proximity-locked (`CanOpenMailBox`), so "strip the master gate" alone cannot work. Mechanism: (a) preferred — grant bot sessions the core's own remote-mailbox affordance (RBAC mailbox permission, mailbox = own guid) and drive the REAL mail-take handlers; (b) fallback — direct server-side mail mutation on the world thread, documented as the program's single P2 exception. E5.2 removes either. | money AND returned-item collection ratio ~100%; zero mail-expiry deletions of bot-owned content (E1.8 emitter watches this) |
| E5.2 | M1 realistic: mailbox GO targeting piggybacked on AH/city visits; removes the M0 affordance | bots collect at mailboxes; ratios unchanged |

### E6 — Banking and inventory hygiene (M)

| Story | What | Acceptance |
| --- | --- | --- |
| E6.1 | IterateItems bank-mask fix (one line) | combined-mask scans see bank items; no regressions |
| E6.2 | `bank space` / `bank items` values | queryable per bot |
| E6.3a | rev (split): autonomous DEPOSIT leg — BankAction off the chat gate, banker-arrival hook, deposit strategic mats under bag pressure | bank rows > 0 (baseline exactly 0); deposits on banker arrival |
| E6.3b | rev: withdraw-for-recipe leg — depends on E7.1 (edge drawn in Section 5) | mats flow bank->bags ahead of craft casts |
| E6.4 | Bank bag slot purchase when bank pressure + budget | bankSlots distribution grows from 0 |
| E6.5 | rev: bank triage — E2.3's logic runs over bank contents; dead stock (mats no profession of the bot consumes) goes to AH or, post-E10.6, guild bank | bank item age distribution stays bounded |

### E7 — Crafting and profession supply chains (L)

| Story | What | Acceptance |
| --- | --- | --- |
| E7.1 | Wire the orphaned CraftRandomItemAction (city idle / vendor-trip trigger; reagent filter exists) | craft casts > 0 (baseline zero); products appear in bags |
| E7.2 | Genuine profession progression — PENDING the D4 decision in Section 6, and rev: expanded to what it actually takes: gate BOTH re-pin sites (AutoMaintenance levelup + factory Refresh); add profession-RANK training (E1.2 index covers profession trainers; GO_TRAIN extended) or progression stalls at 75/150/225; ONE-WAY: re-pinning destroys genuine values — snapshot character_skills first (E0.0 has the query) | no bot stuck at a rank cap with a funded training need; skill values decouple from level*5 |
| E7.3 | Vertical integration: crafters consume own mats, list products not mats | alchemists list potions, pure gatherers list herbs |
| E7.4 | `materials` need + AH reagent buying (needs E8.1/E8.2) | first gatherer-to-crafter transaction — the flagship KPI |
| E7.5 | Deliberate gathering. rev: NEEDS-DRIVEN only — raised by an unmet own-materials basket or a sell-for-gold need with market demand, never free-running (that recreates the pointless-gathering the operator forbade); skill-tier-aware node routing (nodes gate on required skill) | supply rises AND gathered-vs-listed/consumed ratio stays bounded (no hoarding) |

### E8 — Auction house: buy side (M)

| Story | What | Acceptance |
| --- | --- | --- |
| E8.1 | World-thread listings mirror (mutex snapshot) | refresh <1s; zero cross-thread AH touches |
| E8.2 | AhBuyAction, buyout-only. rev: ships its OWN self-dealing guard — the core same-account check only fires for OFFLINE owners, which permanently-online bots never are; resolve owner account via CharacterCache and skip same-account (decision recorded: same-guild siblings allowed, flagged in analytics) | bots win auctions; won items equipped after mail collection; zero same-account trades in the KPI query |
| E8.3 | AutoUpgradeEquip cap (free gear through 19; market-driven from 20) | gear_slot needs appear at scale; AH gear velocity > 0; leveling pace unharmed (race telemetry) |

### E9 — Observability and analytics (continuous)

rev: restructured — E9.1 is no longer a single late story; the schema and
first emitters shipped in E1.8, and every epic ships its own event types.
Frontend panels attach to the epic that produces their data.

| Story | What | Acceptance |
| --- | --- | --- |
| E9.1 | Schema ownership + completeness audit of `aetherion_econ_events`; rev: includes ATTEMPT/FAILURE event types (travel started, listing rejected, deposit unaffordable, no mailbox found) so "trying but failing" is distinguishable from "not trying" — the silent-market failure mode P1 makes likely | audit: every flow type in this BRD has an emitter; ECONOMY tab shows attempts vs completions |
| E9.2a-e | One panel per story, attached to its data producer: market board (E4.4), gold supply curve (E1.7), needs funnel (E1.5), profession flows (E7), richest-bots board (E1.7) | each panel matches its SQL cross-check |
| E9.3 | Prometheus exporter in ai-bridge (metrics from aetherion tables) for Grafana | /metrics exposes econ gauges; zero game-server changes |
| E9.4 | Worldserver OTEL — see decision D7; placeholder pending operator choice | — |

### E10 — Guild economy (L, downstream)

| Story | What | Acceptance |
| --- | --- | --- |
| E10.1a | rev (split, and corrected): SPIKE — activate "+guild" on staging. Known from source: it wires offer/sign/turn-in/tabard ONLY; the charter BUY step is wired solely into the unloaded legacy rpg strategy, so petitions stay 0 without new wiring. Spike confirms and records the decision | spike report: which steps fire, which need wiring |
| E10.1b | Charter purchase wiring under new-RPG (GuildStrategy trigger or GO_PETITION status driving BuyPetitionAction at petitioner NPCs, proximity respected); race-gate the hidden 10g emblem top-up; charter paid with earned 10s | petitions > 0 (baseline zero); first organic guild is a RACE-tab milestone |
| E10.2 | Preset-guild policy for the race (organic-only vs hybrid) — decision D6 | per decision |
| E10.3a | rev: GO_GUILD_BANK targeting — guild VAULTS are GameObjects, not creature bankers, and are not wander targets today; locate + route + arrival hook; spike confirms vault GOs reachable in both factions' capitals | staging bots reach a vault GO |
| E10.3b | Treasury deposits (income share above free-money threshold; guild-bank money opcodes) | guild BankMoney > 0 (baseline zero) |
| E10.4 | Tab purchases by officer bots from pooled readiness | first tab purchase event; 100g-5000g sink ladder active |
| E10.5 | Guild-funded repairs: leader grants rank money-per-day + withdraw-repair right (core mechanic, ranks default 0) | broke members repair on guild coin |
| E10.6 | Item sharing via tabs (deposit surplus, withdraw by matching need) | guild_bank_item flows both directions |
| E10.7 | rev: GUILD GOALS — the operator's "work as a guild toward goals": a per-guild goal ledger (first tab, treasury floor, member-mount fund) that raises members' guild-deposit need while unmet | deposit rates rise while a goal is open, fall when met; goals visible on the GUILDS tab |
| E10.8 | GuildTaskMgr: legacy, do not build on it (dead caller, real-player targeted, faucet rewards violate P1) | decision recorded |

## 5. Sequencing

```
E0.0 (baseline!) -> E0.1-0.4 -> E1 (observe-only, incl. gold + event baselines)
  -> E2.0 (framework) -> E2.1-2.5 (income) -> E3 (sinks, story-by-story)
       -> E4.1a (sell + opportunistic hook) -> E5.1 (mail; hard edge from E4.1a)
            -> E4.1b/c, E4.2, E4.3 -> E8 (buy side)
E6 after E2 (deposit leg standalone); E6.3b after E7.1
E7.1-7.3 after E4; E7.4 after E8.1-8.2; E7.5 after E7.4 (demand exists)
E9 panels attach to their producers throughout
E10 last; E10.3a spike may run early on staging
```

Phase gates: E0.0 query set re-run + E1.7 solvency green on staging, then
production, per the epic DoD.

## 6. Operator decisions (open)

| # | Decision | Options | Default proposal |
| --- | --- | --- | --- |
| D1 | Market unification | neutral AH (deep, 15% goblin cut) vs faction AHs | unify |
| D2 | MailDeliveryDelay | keep 3600s vs shorten | keep; revisit if the loop feels dead |
| D3 | AH at wipe | strictly empty vs tiny seed | strictly empty (P1) |
| D4 | Profession skill pinning (E7.2) | keep synthetic level*5 vs genuine progression (one-way once bots level; needs rank-training work) | genuine, WITH the E7.2 expansion; snapshot first |
| D5 | Free-gear cap (E8.3) | AutoUpgradeEquip level threshold | free through 19 |
| D6 | Preset guilds at the race | organic-only vs hybrid | organic-only post-wipe |
| D7 | rev: OpenTelemetry in the worldserver (operator asked; was wrongly auto-deferred) | (a) E9.3 Prometheus-from-tables only; (b) OTEL C++ SDK in the worldserver (heavy intrusive dependency, per-tick tracing possible) | (a) now, (b) deferred — operator call |
| D8 | Death economics (E3.1) | durability loss on death per vanilla vs keep free release-repair (repair sink becomes a no-op) | vanilla rules |
| D9 | Self-dealing scope (E8.2) | block same-account only vs also same-guild | same-account only, same-guild flagged in analytics |

## 7. Non-functional requirements

- **N1** Needs pass <5ms world-tick delta at 2500 bots, measured against the
  E0.0-snapshotted mean/p99 update diff.
- **N2** No new locks on core structures: world-thread drains + snapshot
  mirrors only.
- **N3** Config-reversible per epic, with the rollback drill in the DoD (E7.2
  exempted and labeled one-way).
- **N4** Patchers marker-idempotent, tested fresh AND patched, per the 3.4
  ownership map.
- **N5** Telemetry writes async, truncate-and-rewrite or watermark-append.

## 8. Risks

| Risk | Mitigation |
| --- | --- |
| Sinks outpace income; fleet goes broke | E2-before-E3 sequencing; E1.7 solvency gates; config fallbacks |
| Silent market misread as "not trying" | E9.1 attempt/failure events; empty-AH is only meaningful with them |
| Thin market, nobody buys | D5 demand lever; E7.4 structural demand |
| Perf regression | E1.2 index; N1 budget measured first |
| Patch anchor churn | 3.4 ownership map; E0.3 idempotency tests |
| HandleBotPackets ignores PROCESS flags | safe only while its call sites stay world-thread-only — documented invariant, re-checked on every upgrade |
| Assembler clobbers economy errands | 3.6 coexistence rules (sweep skips mid-transaction statuses) |
| Mail expiry silently deletes items | E1.8 mail-expiry emitter + E5.1 acceptance |
| Upstream module updates | anchored asserts fail loudly |

## 9. Progress log

| Date | Shipped | Validation |
| --- | --- | --- |
| 2026-08-21 | E0.0-E0.4 (baseline archived to docs/baselines/, ahbot retired by default, Econ namespace, restore-first patch chain, staging = the MacBook instance) | double-apply deterministic; baseline query set archived |
| 2026-08-21 | E1.1-E1.8 (NeedsLedger observe-only: needs pass, trainer index, worst-slots, gold snapshots + bands, econ events schema, destruction emitters, ECON tab) | staging: 1500 gear + 58 repair needs with DBC-correct amounts; mount/training/ammo correctly zero while free paths run; destruction baseline captured (161k items in first minutes - the E2.3 headline) |
| 2026-08-21 | E2.0/E2.1a/E2.2/E2.3+E2.4/E2.5 (IDLE preemption + verdict mirror, chosen-vendor WanderNpc trips, "trash" sell mode with vendor_sell emitter, trade-goods protection, broke-with-sellables verdict) | staging: vendor_sell events flowing (greys only, real money deltas); trade-goods destruction 0 since arming (was thousands); zero AH-quality leakage. E2.5 correlation acceptance PENDING a race-mode staging run - with legacy gold nobody is broke, so the broke-verdict path has no live exercise yet |

| 2026-08-21 | Race-mode wipe REHEARSAL on staging (full wipe-race-start.sh run) | Caught 3 script bugs pre-production (bash-3.2 mapfile, pipefail on empty prune dir, hardcoded host in warning block). Post-wipe verdict: 500/500 bots at exactly level 1 (zero re-rolls - the dispatcher works), money 0-4 copper (all grants gated), 0 DKs online, race clock running, brackets off. First genuinely unfunded needs appeared (8 hunters broke for ammo) - the E1 ledger in its real regime. E2.5 broke-selling correlation now observable as trash accumulates |

| 2026-08-21 | E2.1b (far-vendor leg: startup vendor-spawn index, nearest-vendor resolution in the verdict pass, GoCamp walk + nearby-scan completion) | staging (race realm): distinct sellers 3 -> 15 in one soak window; money supply 308c -> 1361c across 130 holders, all earned; L2 cohort growing with zero knockbacks. E2 epic COMPLETE; E2.5 correlation now continuously green (every seller holds coppers) |

| 2026-08-21 | Parallel increment: E3.1 (all 8 free-repair sites gated, repair_paid emitter, vendor-visit repair dispatch; masters' alts exempt), E3.4a (food cheat dropped), E4.1a (AhSellAction - verified packet path, opportunistic auctioneer-arrival posting, agent-built + reviewed), E4.4 (AuctionHouseScript hooks: ah_listed/ah_sold/ah_bought/ah_expired), E5.1 (world-thread mail collection, the single documented P2 exception; COD never auto-paid; E5.2 retires it), frontend market board (flow counts, live listings by seller, top seller/buyer, income-rate sparkline, top earners) | all compiled + deployed on the race-rehearsal staging realm; soak baseline measured: fleet income ~3.6s/hr at L1-2, money supply 29s->103s in 35min, L2+ cohort 173->429. AH/repair/mail watchers armed - validation completes at simulation speed as bots reach vendactioneers and take damage |

| 2026-08-21 | E8.1 + E8.2 (world-thread listings mirror with per-listing owner-account; AhBuyAction: buyout-only, one purchase per auctioneer visit, gear-budget gated, upgrade-oracle evaluated, own same-account guard per D9) | compiled first try, deployed armed on staging; the market is now two-sided |
| 2026-08-21 | E9.3 Prometheus exporter (agent-built): GET /metrics on the bridge content-negotiates - Prometheus Accept gets exposition format (22 aetherion_* series: bots online, gold supply, level bands, needs funded/unfunded, econ event counters, AH listings/sellers, race firsts), the dashboard's generic Accept keeps its JSON contract. Bridge healthcheck timeout widened 5s->20s (fresh-interpreter probe under laptop compile load) | both faces curl-verified on staging; Grafana needs only a scrape job at the bridge on the compose network |

E2 is closed; E3.1/E3.4a/E4.1a/E4.4/E5.1/E8 are deployed-pending-soak. Next
build items: E3.2 paid training (needs GO_TRAIN), E5.2 real mailbox visits,
E6 banking, E7.1 crafting. Next validation items (simulation-speed): first
listing landing, first paid repair, first collected sale gold, FIRST COMPLETED
BOT-TO-BOT TRADE (ah_sold + ah_bought pair) - the program's flagship KPI.

| 2026-08-22 | PRODUCTION ROLLOUT (no wipe): E0-E8 + E3.2 + E5.2 + E7 (craft/reagent-buy/keep-own-reagents) + E6.3a (bank deposits) deployed to the live 2500-bot realm via deploy-econ.sh; all Econ keys armed persistently | First needs pass at full scale: all 2500 bots carry a materials need (crafting had never run), 112 repair needs. FIRST CRAFTS IN REALM HISTORY within seconds of the E7 deploy (bandages, armor kits from hoarded cloth/leather). Windfall tracking live: five veterans mailed 100g each, mailbox walks pending their next idle. Realm survived four worldserver restarts today with zero errors |

| 2026-08-22 | Production firsts, same evening: first bank deposit in realm history (Corrioa - who banked her own enchanting rod, exposing the tools-vs-reagents gap, fixed same hour: CraftPlanner now surfaces Totem tools and both keep-filters honor them); first bot-authored auction listings (Ameterena, level 55: four stacks - sharpening stones, nectar, bacon, grainbread - 4/4 attempts landed, deposits paid) | The market has genuine supply from a genuine seller. Remaining watched firsts: windfall mailbox walks, first completed bot-to-bot trade |

## 10. Relationship to the race

The economy makes the race legible: wealth, gear provenance, guild treasuries,
market share join levels on the boards. The race makes the economy honest:
every coin earned from level 1. Recommended order unchanged: E0-E5 built and
validated on staging, then the wipe, so the simulation starts with a
functioning market on day 0.
