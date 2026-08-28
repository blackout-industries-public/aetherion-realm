#!/usr/bin/env bash
# Apply the tuned realm configuration on top of the freshly generated .dist files.
# Idempotent and re-runnable; run it after every upgrade, since upstream may add keys.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

BOTS=${BOT_POPULATION:-50}

[[ -f $CONF/worldserver.conf ]] || die "configs not generated yet; run 'docker compose up -d' once first"
mkdir -p "$CONF/modules"

# Module configs are only shipped as .dist; a real .conf is what survives an upgrade.
for m in playerbots mod_ahbot; do
    [[ -f $CONF/modules/$m.conf ]] || cp "$CONF/modules/$m.conf.dist" "$CONF/modules/$m.conf"
done

log "worldserver"
set_conf "$CONF/worldserver.conf" "RealmID" "1"
# SOAP is the only non-interactive way to drive the worldserver for health checks and
# backups; bound to the container and republished on loopback only.
set_conf "$CONF/worldserver.conf" "SOAP.Enabled" "1"
set_conf "$CONF/worldserver.conf" "SOAP.IP" "\"0.0.0.0\""
# db-import owns migrations. Leaving this on races two containers against one schema.
set_conf "$CONF/worldserver.conf" "Updates.EnableDatabases" "0"

# The dashboard reads positions from the database, and the world server only writes
# them on this timer. At the 15-minute default the map shows a quarter-hour-old
# snapshot no matter how often it polls, which reads as bots standing still.
# 60s costs ~1500 row updates a minute at full population - measure tick diff before
# lowering it further.
set_conf "$CONF/worldserver.conf" "PlayerSaveInterval" "${PLAYER_SAVE_INTERVAL_MS:-60000}"
# Gear gating lives in the assembler's scaled-appetite math (party average
# vs the seeded floors), not at the door: the portal hard-check made one
# green member a veto and shed stragglers at the threshold. Off by default.
set_conf "$CONF/worldserver.conf" "DungeonAccessRequirements.PortalAvgIlevelCheck" "${PORTAL_ILVL_CHECK:-0}"
# Without this, battlegrounds run but pvpstats_battlegrounds and pvpstats_players stay
# empty, so no match ever has a score, a bracket or a winner.
set_conf "$CONF/worldserver.conf" "Battleground.StoreStatistics.Enable" "${BG_STORE_STATS:-1}"

set_conf "$CONF/worldserver.conf" "Ra.Enable" "1"
set_conf "$CONF/worldserver.conf" "Ra.IP" "\"0.0.0.0\""
set_conf "$CONF/worldserver.conf" "Ra.MinLevel" "3"

# The module reads these from worldserver.conf but ships no entries for them.
set_conf "$CONF/worldserver.conf" "Playerbots.Updates.EnableDatabases" "1"
set_conf "$CONF/worldserver.conf" "PlayerbotsDatabase.WorkerThreads" "1"
set_conf "$CONF/worldserver.conf" "PlayerbotsDatabase.SynchThreads" "1"

log "playerbots: population ${BOTS}, level brackets on"
PB=$CONF/modules/playerbots.conf
set_conf "$PB" "AiPlayerbot.Enabled" "1"
set_conf "$PB" "AiPlayerbot.RandomBotAutologin" "1"
set_conf "$PB" "AiPlayerbot.MinRandomBots" "$BOTS"
set_conf "$PB" "AiPlayerbot.MaxRandomBots" "$BOTS"
# Without this every bot drifts to 80 and the low-level world empties out (BRD s9).
# The shipped 9 brackets already match the BRD's requested distribution.
set_conf "$PB" "AiPlayerbot.LevelBrackets.Enabled" "1"
set_conf "$PB" "AiPlayerbot.RandomBotMinLevel" "1"
set_conf "$PB" "AiPlayerbot.RandomBotMaxLevel" "80"
# Bots keep questing while nobody is logged in, so the world has history when you arrive.
set_conf "$PB" "AiPlayerbot.DisabledWithoutRealPlayer" "0"
set_conf "$PB" "AiPlayerbot.AllowGuildBots" "1"
set_conf "$PB" "AiPlayerbot.RandomBotInvitePlayer" "1"
# Bots forming their own parties. Off upstream, which is why a 1500-bot realm had
# exactly one group in it.
set_conf "$PB" "AiPlayerbot.RandomBotGroupNearby" "${BOT_GROUP_NEARBY:-1}"
# Party-size ambition. Leader-heavy on purpose: two-bot parties can never run a
# dungeon, so LFG has nothing to work with. Must total 100 with the LEADER_5 remainder.
set_conf "$PB" "AiPlayerbot.Grouper.SoloPct" "${GROUPER_SOLO:-10}"
set_conf "$PB" "AiPlayerbot.Grouper.MemberPct" "${GROUPER_MEMBER:-50}"
set_conf "$PB" "AiPlayerbot.Grouper.Leader2Pct" "${GROUPER_L2:-5}"
set_conf "$PB" "AiPlayerbot.Grouper.Leader3Pct" "${GROUPER_L3:-5}"
set_conf "$PB" "AiPlayerbot.Grouper.Leader4Pct" "${GROUPER_L4:-10}"

# Background party assembly - forms complete parties regardless of distance.
set_conf "$PB" "AiPlayerbot.Party.Enabled" "${PARTY_ASSEMBLER:-1}"
set_conf "$PB" "AiPlayerbot.Party.IntervalMs" "${PARTY_INTERVAL_MS:-45000}"
set_conf "$PB" "AiPlayerbot.Party.TargetSize" "${PARTY_SIZE:-5}"
set_conf "$PB" "AiPlayerbot.Party.MaxParties" "${PARTY_MAX:-80}"
set_conf "$PB" "AiPlayerbot.Party.PerTick" "${PARTY_PER_TICK:-3}"
set_conf "$PB" "AiPlayerbot.Party.MinLevel" "${PARTY_MIN_LEVEL:-15}"
set_conf "$PB" "AiPlayerbot.Party.LevelSpread" "${PARTY_LEVEL_SPREAD:-4}"
set_conf "$PB" "AiPlayerbot.Party.Teleport" "${PARTY_TELEPORT:-1}"
set_conf "$PB" "AiPlayerbot.Party.SameMapOnly" "${PARTY_SAME_MAP:-1}"
set_conf "$PB" "AiPlayerbot.Party.GatherRange" "${PARTY_GATHER_RANGE:-400}"
set_conf "$PB" "AiPlayerbot.Party.ArriveRange" "${PARTY_ARRIVE_RANGE:-60}"
set_conf "$PB" "AiPlayerbot.Party.MaxTripTicks" "${PARTY_MAX_TRIP_TICKS:-40}"
set_conf "$PB" "AiPlayerbot.Party.StallTicks" "${PARTY_STALL_TICKS:-2}"
set_conf "$PB" "AiPlayerbot.Party.InsideTicks" "${PARTY_INSIDE_TICKS:-40}"
set_conf "$PB" "AiPlayerbot.Party.InsideTicksRaidMult" "${PARTY_INSIDE_RAID_MULT:-3}"
set_conf "$PB" "AiPlayerbot.Party.SweepPerTick" "${PARTY_SWEEP_PER_TICK:-25}"
set_conf "$PB" "AiPlayerbot.Party.HuntRange" "${PARTY_HUNT_RANGE:-8}"
set_conf "$PB" "AiPlayerbot.Party.NearestChoices" "${PARTY_NEAREST:-4}"
set_conf "$PB" "AiPlayerbot.Party.FootRange" "${PARTY_FOOT_RANGE:-1200}"
set_conf "$PB" "AiPlayerbot.Party.PortalPct" "${PARTY_PORTAL_PCT:-50}"
set_conf "$PB" "AiPlayerbot.Party.RaidPct" "${PARTY_RAID_PCT:-20}"
set_conf "$PB" "AiPlayerbot.Party.RaidSize" "${PARTY_RAID_SIZE:-10}"
# Raid formats: share of raids seated as 25s, and share of raid trips that
# attempt heroic where the destination offers it and access checks pass.
set_conf "$PB" "AiPlayerbot.Party.Raid25Pct" "${PARTY_RAID25_PCT:-25}"
set_conf "$PB" "AiPlayerbot.Party.RaidHeroicPct" "${PARTY_RAID_HEROIC_PCT:-15}"
# The muster: hold one faction's capped bots back from party formation
# until 25 can seat, so full-size raids actually occur. 0 disables.
set_conf "$PB" "AiPlayerbot.Party.MusterEveryMin" "${PARTY_MUSTER_EVERY_MIN:-45}"
set_conf "$PB" "AiPlayerbot.Party.MusterTimeoutMin" "${PARTY_MUSTER_TIMEOUT_MIN:-12}"
# Wipe determination: full-party falls a run absorbs (revived at the door,
# fresh clock) before the group calls it. 0 = first wipe ends progress.
set_conf "$PB" "AiPlayerbot.Party.WipeRetries" "${PARTY_WIPE_RETRIES:-3}"
# Determination that has to be earned. Every encounter a run puts down buys it
# one more wipe (to a cap) and one more block of dwell clock (to a cap), so a
# raid still killing things keeps going and a raid killing nothing does not.
set_conf "$PB" "AiPlayerbot.Party.WipeBonusPerBoss" "${PARTY_WIPE_BONUS_PER_BOSS:-1}"
set_conf "$PB" "AiPlayerbot.Party.WipeBonusCap" "${PARTY_WIPE_BONUS_CAP:-4}"
set_conf "$PB" "AiPlayerbot.Party.ProgressExtendTicks" "${PARTY_PROGRESS_EXTEND_TICKS:-10}"
set_conf "$PB" "AiPlayerbot.Party.ProgressExtendMax" "${PARTY_PROGRESS_EXTEND_MAX:-4}"
# How far under a door's item-level floor a party will still try. At the floor
# the appetite is whole and it ramps to nothing this far below; past that the
# destination is named as out of reach instead of quietly attempted.
set_conf "$PB" "AiPlayerbot.Party.GearStretch" "${PARTY_GEAR_STRETCH:-20}"
# Ticks the summon at the door gets before the party is taken in from where it
# stands. At some doors the arrival test never comes true and the party waits
# out its whole budget: Ulduar 32 runs reached the door for 4 entries, Uldaman
# 6 for none. Four is well clear of the one or two an ordinary door takes.
set_conf "$PB" "AiPlayerbot.Party.SummonTicks" "${PARTY_SUMMON_TICKS:-4}"
# Ticks a run with every boss dead or given up on is held before it is closed
# out, rather than wandering trash for the rest of a 90-minute clock.
set_conf "$PB" "AiPlayerbot.Party.ExhaustGrace" "${PARTY_EXHAUST_GRACE:-4}"
# The beat between pulls. After a fight ends the leader is held where it stands
# for up to this many ticks while anybody is still walking back, hurt, or out of
# mana, so the party loots and drinks instead of dragging the next pack onto a
# fight that has only just finished. Costs nothing when the party is already fit.
set_conf "$PB" "AiPlayerbot.Party.SettleTicks" "${PARTY_SETTLE_TICKS:-2}"
set_conf "$PB" "AiPlayerbot.Party.RegroupRange" "${PARTY_REGROUP_RANGE:-40}"
set_conf "$PB" "AiPlayerbot.Party.ShortAbortLimit" "${PARTY_SHORT_ABORT_LIMIT:-3}"
set_conf "$PB" "AiPlayerbot.Party.CollectorPct" "${PARTY_COLLECTOR_PCT:-60}"
set_conf "$PB" "AiPlayerbot.Party.QueueLfg" "${PARTY_QUEUE_LFG:-1}"
set_conf "$PB" "AiPlayerbot.Party.TravelToDungeon" "${PARTY_TRAVEL:-1}"

# World PvP. The weight is how often a bot goes looking for a fight, against
# DoQuest at 60; the strategy is what lets it fight players properly.
set_conf "$PB" "AiPlayerbot.Pvp.Enabled" "${PVP_ENABLED:-1}"

# Realism flags, from the 2026-08-21 disabled-feature audit (18 agents, each claim
# independently verified against module source). All six are init-time or roll-time
# costs, nothing per-tick. RandomBotEmote deliberately NOT enabled: its receive-emote
# triggers can cascade in packed capitals.
# Activity throttle: with the shipped 10, only ~30% of bots run their AI at all -
# the direct cause of 1679 bots idling and quests going unfinished. Step the ladder
# (10 -> 25 -> 40) watching the world update diff; SmartScale caps the damage.
# Four continents, ~70 instances and five battlegrounds updated serially on one
# thread is why the throttle had to freeze 90% of the realm. Parallel maps need the
# event-cache lock patch (patches/llm/patch_eventlock.py) - do not raise this without it.
set_conf "$CONF/worldserver.conf" "MapUpdate.Threads" "${MAP_THREADS:-4}"
# Aspiration, not a promise: SmartScale still governs against the tick budget, so
# over-provisioning is safe - the multiplier throttles automatically.
set_conf "$PB" "AiPlayerbot.BotActiveAlone" "${BOT_ACTIVE_ALONE:-40}"
# SmartScale multiplies BotActiveAlone by (1 - (maxDiff-floor)/(ceiling-floor)), where
# maxDiff is the MAX of the last 500 world-update samples. At the shipped 50/200 band a
# single 200ms+ spike pins the multiplier to zero for the whole window - measured
# effective activity was 0-1% regardless of BotActiveAlone, which made it a dead knob.
# 100/300 keeps the hard cutoff (zero at 300ms) while letting a ~160ms mean tick run
# bots at a real duty cycle.
set_conf "$PB" "AiPlayerbot.botActiveAloneSmartScaleDiffLimitfloor" "${SMARTSCALE_FLOOR:-100}"
set_conf "$PB" "AiPlayerbot.botActiveAloneSmartScaleDiffLimitCeiling" "${SMARTSCALE_CEILING:-300}"
set_conf "$PB" "AiPlayerbot.Party.DriveGroupedBots" "${PARTY_DRIVE_GROUPED:-1}"
set_conf "$PB" "AiPlayerbot.EnableGreet" "${BOT_GREET:-1}"
set_conf "$PB" "AiPlayerbot.LootGreedRollLevel" "${LOOT_GREED:-1}"
set_conf "$PB" "AiPlayerbot.LootRollDisenchant" "${LOOT_DE:-1}"
set_conf "$PB" "AiPlayerbot.PreferClassArmorType" "${GEAR_ARMOR_TYPE:-1}"
set_conf "$PB" "AiPlayerbot.PreferredSpecWeapons" "${GEAR_SPEC_WEAPONS:-1}"
set_conf "$PB" "AiPlayerbot.RandomGearLoweringChance" "${GEAR_LOWERING:-0.15}"

# Battlegrounds. RandomBotJoinBG only gives bots the "bg" strategy; nothing queues them
# without RandomBotAutoJoinBG, which is why every battleground table was empty while
# 68 characters held honour earned in the open world.
set_conf "$PB" "AiPlayerbot.RandomBotAutoJoinBG" "${BG_AUTOJOIN:-1}"
set_conf "$PB" "AiPlayerbot.RandomBotAutoJoinBGWSCount" "${BG_WSG:-2}"
set_conf "$PB" "AiPlayerbot.RandomBotAutoJoinBGABCount" "${BG_AB:-2}"
set_conf "$PB" "AiPlayerbot.RandomBotAutoJoinBGEYCount" "${BG_EY:-1}"
set_conf "$PB" "AiPlayerbot.RandomBotAutoJoinBGAVCount" "${BG_AV:-1}"
set_conf "$PB" "AiPlayerbot.RandomBotAutoJoinBGICCount" "${BG_IC:-1}"
set_conf "$PB" "AiPlayerbot.RpgStatusProbWeight.OutdoorPvp" "${PVP_WEIGHT:-30}"
set_conf "$PB" "AiPlayerbot.RandomBotJoinLfg" "${BOT_JOIN_LFG:-1}"
set_conf "$PB" "AiPlayerbot.RandomBotSuggestDungeons" "${BOT_SUGGEST_DUNGEONS:-1}"
set_conf "$PB" "AiPlayerbot.RandomBotGuildCount" "${BOT_GUILD_COUNT:-60}"
set_conf "$PB" "AiPlayerbot.RandomBotGuildSizeMax" "${BOT_GUILD_SIZE_MAX:-25}"

# Phase 2 bridge. Off unless LLM_ENABLED=1 is passed, so a plain configure run can
# never silently switch inference on.
log "llm bridge: ${LLM_ENABLED:-0}"
set_conf "$PB" "AiPlayerbot.Llm.Enabled" "${LLM_ENABLED:-0}"
set_conf "$PB" "AiPlayerbot.Llm.Host" "\"${LLM_BRIDGE_HOST:-ai-bridge}\""
set_conf "$PB" "AiPlayerbot.Llm.Port" "${LLM_BRIDGE_PORT:-8090}"
set_conf "$PB" "AiPlayerbot.Llm.TimeoutMs" "${LLM_TIMEOUT_MS:-15000}"
set_conf "$PB" "AiPlayerbot.Llm.MaxInFlight" "${LLM_MAX_INFLIGHT:-8}"
set_conf "$PB" "AiPlayerbot.Llm.ClaimWindowMs" "${LLM_CLAIM_WINDOW_MS:-10000}"
# Do not generate chat nobody is present to receive.
set_conf "$PB" "AiPlayerbot.Llm.RequireHumanWitness" "${LLM_REQUIRE_WITNESS:-1}"
set_conf "$PB" "AiPlayerbot.Llm.SayRange" "${LLM_SAY_RANGE:-45}"
set_conf "$PB" "AiPlayerbot.Llm.SameFactionOnly" "${LLM_SAME_FACTION:-1}"

# Which channels bots react to. Exactly one bot answers any given message.
set_conf "$PB" "AiPlayerbot.Llm.ReactWhisper" "${LLM_REACT_WHISPER:-1}"
set_conf "$PB" "AiPlayerbot.Llm.ReactParty" "${LLM_REACT_PARTY:-1}"
set_conf "$PB" "AiPlayerbot.Llm.ReactGuild" "${LLM_REACT_GUILD:-1}"
set_conf "$PB" "AiPlayerbot.Llm.ReactSay" "${LLM_REACT_SAY:-1}"
# Percent, not certainty: a public channel can hold every bot on the realm.
set_conf "$PB" "AiPlayerbot.Llm.ChannelReplyChance" "${LLM_CHANNEL_CHANCE:-25}"
set_conf "$PB" "AiPlayerbot.Llm.SayReplyChance" "${LLM_SAY_CHANCE:-25}"

# Unprompted bot chatter, and how many bots may answer one another before it stops.
set_conf "$PB" "AiPlayerbot.Llm.AmbientEnabled" "${LLM_AMBIENT:-0}"
set_conf "$PB" "AiPlayerbot.Llm.AmbientIntervalMs" "${LLM_AMBIENT_INTERVAL_MS:-300000}"
set_conf "$PB" "AiPlayerbot.Llm.AmbientMaxDepth" "${LLM_AMBIENT_DEPTH:-1}"
# Local /say by default: it happens visibly next to the player rather than
# scrolling past in a zone-wide channel.
set_conf "$PB" "AiPlayerbot.Llm.AmbientUseSay" "${LLM_AMBIENT_SAY:-1}"

# Greet returning players, and react to things that actually happened.
set_conf "$PB" "AiPlayerbot.Llm.GuildAdEnabled" "${LLM_GUILD_ADS:-1}"
set_conf "$PB" "AiPlayerbot.Llm.GuildAdIntervalMs" "${LLM_GUILD_AD_INTERVAL_MS:-420000}"
set_conf "$PB" "AiPlayerbot.Llm.GreetOnLogin" "${LLM_GREET:-1}"
set_conf "$PB" "AiPlayerbot.Llm.GreetDelayMs" "${LLM_GREET_DELAY_MS:-15000}"
set_conf "$PB" "AiPlayerbot.Llm.EventsEnabled" "${LLM_EVENTS:-1}"
set_conf "$PB" "AiPlayerbot.Llm.EventChanceLevelUp" "${LLM_EVENT_LEVELUP:-100}"
set_conf "$PB" "AiPlayerbot.Llm.EventChanceDeath" "${LLM_EVENT_DEATH:-40}"
set_conf "$PB" "AiPlayerbot.Llm.EventChanceLoot" "${LLM_EVENT_LOOT:-35}"

log "ahbot: ${AHBOT_ENABLED:-0}"
AH=$CONF/modules/mod_ahbot.conf
# Economy BRD E0.1 (P1, no artificial economy): the vendor bot is retired by
# default - the AH is bot-driven or empty. AHBOT_ENABLED=1 re-enables it.
set_conf "$AH" "AuctionHouseBot.EnableSeller" "${AHBOT_ENABLED:-0}"
set_conf "$AH" "AuctionHouseBot.EnableBuyer" "${AHBOT_ENABLED:-0}"
# Start deliberately thin. An AH that restocks faster than players consume is a
# vending machine and removes any reason to play the economy (BRD s14).
set_conf "$AH" "AuctionHouseBot.ItemsPerCycle" "50"
set_conf "$AH" "AuctionHouseBot.LootItems" "1"
set_conf "$AH" "AuctionHouseBot.LootTradeGoods" "1"
set_conf "$AH" "AuctionHouseBot.VendorItems" "0"
set_conf "$AH" "AuctionHouseBot.VendorTradeGoods" "0"
set_conf "$AH" "AuctionHouseBot.No_Bind" "1"
set_conf "$AH" "AuctionHouseBot.Bind_When_Picked_Up" "0"
[[ -n ${AHBOT_ACCOUNT_ID:-} ]] && set_conf "$AH" "AuctionHouseBot.Account" "$AHBOT_ACCOUNT_ID"
[[ -n ${AHBOT_CHAR_GUID:-} ]]   && set_conf "$AH" "AuctionHouseBot.GUID"    "$AHBOT_CHAR_GUID"

# Leveling pace, 1.0 = authentic. The race dial: raise it to compress the
# weeks-long 1..80 climb without touching combat balance.
set_conf "$PB" "AiPlayerbot.RandomBotXPRate" "${BOT_XP_RATE:-1}"

# Economy BRD E0.2: the Econ namespace, everything default-off so a plain
# configure run never arms economy behavior. E1 keys are observe-only.
set_conf "$PB" "AiPlayerbot.Econ.Needs.Enabled" "${ECON_NEEDS:-0}"
set_conf "$PB" "AiPlayerbot.Econ.Needs.TickMs" "${ECON_NEEDS_TICK_MS:-6000}"
set_conf "$PB" "AiPlayerbot.Econ.Needs.Shards" "${ECON_NEEDS_SHARDS:-10}"
set_conf "$PB" "AiPlayerbot.Econ.Events.Enabled" "${ECON_EVENTS:-0}"
# E2: the vendor faucet. Preempt gates the IDLE errand dispatch; Vendor gates
# both the errand verdict and the sell-on-arrival; ProtectTradeGoods stops the
# factory reset destroying the economy's raw supply.
set_conf "$PB" "AiPlayerbot.Econ.Preempt.Enabled" "${ECON_PREEMPT:-0}"
set_conf "$PB" "AiPlayerbot.Econ.Vendor.Enabled" "${ECON_VENDOR:-0}"
set_conf "$PB" "AiPlayerbot.Econ.Vendor.FreeSlotsPct" "${ECON_VENDOR_FREE_PCT:-20}"
set_conf "$PB" "AiPlayerbot.Econ.Vendor.BrokeMinValue" "${ECON_VENDOR_BROKE_MIN:-500}"
set_conf "$PB" "AiPlayerbot.Econ.Vendor.FarMaxYards" "${ECON_VENDOR_FAR_YARDS:-3000}"
set_conf "$PB" "AiPlayerbot.Econ.ProtectTradeGoods" "${ECON_PROTECT_GOODS:-0}"
# E3.1 sink: repairs cost money for masterless bots; the free path stays for
# the operator's alts and as the rollback.
set_conf "$PB" "AiPlayerbot.Econ.PaidRepairs" "${ECON_PAID_REPAIRS:-0}"
# E3.2 sink: class training costs money; the trainer errand pays the bill.
set_conf "$PB" "AiPlayerbot.Econ.PaidTraining" "${ECON_PAID_TRAINING:-0}"
# E7.1: idle-time crafting of focus-free recipes from carried reagents.
set_conf "$PB" "AiPlayerbot.Econ.Craft.Enabled" "${ECON_CRAFT:-0}"
# E6.3a: surplus trade goods bank at banker visits; own reagents stay in bags.
set_conf "$PB" "AiPlayerbot.Econ.Bank.Enabled" "${ECON_BANK:-0}"
set_conf "$PB" "AiPlayerbot.Econ.Bank.MaxPerVisit" "${ECON_BANK_MAX_PER_VISIT:-6}"
set_conf "$PB" "AiPlayerbot.Econ.Guild.TitheFloorGold" "${ECON_GUILD_TITHE_FLOOR:-40}"
set_conf "$PB" "AiPlayerbot.Econ.Guild.TithePct" "${ECON_GUILD_TITHE_PCT:-10}"
# E4.2: a bag holding this many listable items triggers a deliberate AH trip.
set_conf "$PB" "AiPlayerbot.Econ.Ah.MinItemsForTrip" "${ECON_AH_TRIP_MIN:-3}"
# E4.1a: bots list auction-worthy goods when they reach an auctioneer.
set_conf "$PB" "AiPlayerbot.Econ.Ah.Enabled" "${ECON_AH:-0}"
set_conf "$PB" "AiPlayerbot.Econ.Ah.MaxPerVisit" "${ECON_AH_MAX_PER_VISIT:-4}"
# E5.1 interim mail collection (the single deliberate P2 exception); E5.2's
# real mailbox visits will retire it.
set_conf "$PB" "AiPlayerbot.Econ.RemoteMail" "${ECON_REMOTE_MAIL:-0}"
# E8.2: buyout-only demand side - bots shop the mirror for gear upgrades they
# can afford, one purchase per auctioneer visit.
set_conf "$PB" "AiPlayerbot.Econ.Ah.Buy.Enabled" "${ECON_AH_BUY:-0}"
# E5.2: real mailbox walks; when armed it supersedes RemoteMail collection.
set_conf "$PB" "AiPlayerbot.Econ.Mailbox.Enabled" "${ECON_MAILBOX:-0}"
# E7.5: needs-driven gathering trips - never free-running; a bot walks to
# nodes only with a reagent shortfall or an unfunded need to farm toward.
set_conf "$PB" "AiPlayerbot.Econ.Gather.Enabled" "${ECON_GATHER:-0}"
# Both of these were read by the ledger and written by nothing, so they sat on
# their compiled-in defaults while the worldserver logged them as missing every
# boot. Values here are those same defaults - this makes them a knob, not a change.
set_conf "$PB" "AiPlayerbot.Econ.Gather.MaxTierGap" "${ECON_GATHER_TIER_GAP:-150}"
set_conf "$PB" "AiPlayerbot.Econ.Guild.PlentyStacks" "${ECON_GUILD_PLENTY_STACKS:-3}"
# Persona duty scale: global multiplier over each archetype's errand
# appetite (farmer 65 / merchant 50 / hunter 30 / adventurer 22 / warlord 10).
set_conf "$PB" "AiPlayerbot.Econ.Duty.Scale" "${ECON_DUTY_SCALE:-150}"
# E10: the hunter persona goes after named rare spawns. The persona share is
# carved out of the adventurer band (~6% of the fleet); MaxConcurrent caps how
# many of them are walking cross-zone at once, so the economy errands and party
# formation keep the idle time they were tuned against.
set_conf "$PB" "AiPlayerbot.Econ.Rare.Enabled" "${ECON_RARE:-0}"
set_conf "$PB" "AiPlayerbot.Econ.Rare.MaxConcurrent" "${ECON_RARE_MAX:-60}"
set_conf "$PB" "AiPlayerbot.Econ.Rare.FarMaxYards" "${ECON_RARE_FAR_YARDS:-2000}"
# Levels below the bot at which a rare stops being worth the walk.
set_conf "$PB" "AiPlayerbot.Econ.Rare.MaxLevelGap" "${ECON_RARE_LEVEL_GAP:-25}"
# E11 gear market. Rescue keeps still-tradeable green-or-better gear out of the
# factory's inventory shredder so it can reach an auctioneer; measured, that
# shredder was destroying 1030 such pieces a day while the whole fleet listed
# ten. RescueMax caps how many one clear may spare, because a rescued piece
# costs a bag slot until it sells.
set_conf "$PB" "AiPlayerbot.Econ.Gear.Rescue" "${ECON_GEAR_RESCUE:-0}"
set_conf "$PB" "AiPlayerbot.Econ.Gear.RescueMax" "${ECON_GEAR_RESCUE_MAX:-6}"
# The shopping trip: a bot whose funded gear need names something live on the
# market walks to an auctioneer. Weakest claim on the realm - it only ever takes
# a bot every other errand declined - and bounded by a realm-wide ceiling. The
# trip lasts while the reason does; there is deliberately no per-bot timer, one
# having already been measured deleting the errand a minute after granting it.
set_conf "$PB" "AiPlayerbot.Econ.Gear.Shop.Enabled" "${ECON_GEAR_SHOP:-0}"
set_conf "$PB" "AiPlayerbot.Econ.Gear.Shop.MaxConcurrent" "${ECON_GEAR_SHOP_MAX:-80}"
# How many known recipes a crafter ranks before casting. Twelve truncated the
# list in spell-map order, so a tailor chose among an arbitrary twelve.
set_conf "$PB" "AiPlayerbot.Econ.Craft.Scan" "${ECON_CRAFT_SCAN:-40}"
# E3.4a: the food cheat is a hidden faucet-equivalent; dropping 'food' makes
# hunger real. Default keeps today's mask - staging arms the reduced one.
set_conf "$PB" "AiPlayerbot.BotCheats" "\"${BOT_CHEATS:-food,taxi,raid}\""

# Race mode: every bot starts at level 1 and keeps only what it earns. Overrides
# the bracket enforcement set above; the C++ side (patch_racemode) is inert until
# DisableRandomLevels flips here. DKs are parked because they cannot start below
# 55 and would top every board on day one. Armed by scripts/wipe-race-start.sh
# writing RACE_MODE=1 into overlay/.env, so a plain configure run keeps it armed.
if [[ ${RACE_MODE:-0} == 1 ]]; then
    log "RACE MODE armed: natural leveling, brackets off, DKs parked"
    set_conf "$PB" "AiPlayerbot.DisableRandomLevels" "1"
    set_conf "$PB" "AiPlayerbot.RandombotStartingLevel" "${RACE_START_LEVEL:-1}"
    set_conf "$PB" "AiPlayerbot.LevelBrackets.Enabled" "0"
    set_conf "$PB" "AiPlayerbot.DisableDeathKnightLogin" "${RACE_PARK_DKS:-1}"
    # An AH stocked by the vendor bot would hand the racers an economy they did
    # not build - and after a wipe AHBOT_CHAR_GUID points at whichever new bot
    # inherited the guid. Off unless deliberately re-enabled.
    set_conf "$AH" "AuctionHouseBot.EnableSeller" "${AHBOT_IN_RACE:-0}"
    set_conf "$AH" "AuctionHouseBot.EnableBuyer" "${AHBOT_IN_RACE:-0}"
fi

# MOTD moved out of worldserver.conf into acore_auth.motd; a Motd key in the config
# file is silently ignored by this core revision. tmp-and-move instead of sed -i,
# which differs between GNU and BSD sed (this script also runs on macOS staging).
sed '/^Motd[[:space:]]*=/d' "$CONF/worldserver.conf" > "$CONF/worldserver.conf.tmp" \
    && mv "$CONF/worldserver.conf.tmp" "$CONF/worldserver.conf"

log "realmlist -> ${REALM_ADDRESS}"
dc exec -T ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD" acore_auth -e \
  "UPDATE realmlist SET name='${REALM_NAME}', address='${REALM_ADDRESS}', localAddress='${REALM_ADDRESS}', port=${DOCKER_WORLD_EXTERNAL_PORT:-8085} WHERE id=1;
   INSERT INTO motd (realmid, text) VALUES (1, 'Welcome to ${REALM_NAME}.')
   ON DUPLICATE KEY UPDATE text=VALUES(text);"

# Facts the dashboard needs that live outside the game database: pinned revisions and
# realm identity. Written here so there is one source of truth, and deliberately a
# safe subset - overlay/.env holds credentials and is never exposed to the frontend.
cat > "$CONF/realm-facts.json" <<JSON
{
  "realm": "${REALM_NAME:-Aetherion}",
  "address": "${REALM_ADDRESS:-unknown}",
  "botPopulation": ${BOTS},
  "llmModel": "${MODEL_INTERACTIVE:-}",
  "llmBaseUrl": "${LLM_BASE_URL:-}",
  "pins": [
    { "name": "AzerothCore (Playerbot fork)", "commit": "${AC_COMMIT:-}" },
    { "name": "mod-playerbots", "commit": "${PLAYERBOTS_COMMIT:-}" },
    { "name": "mod-ah-bot", "commit": "${AHBOT_COMMIT:-}" }
  ],
  "generatedAt": $(date +%s)
}
JSON
chmod 644 "$CONF/realm-facts.json"
log "wrote $CONF/realm-facts.json"

log "configure complete - restart worldserver to apply"
