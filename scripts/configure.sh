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
set_conf "$PB" "AiPlayerbot.Party.MaxParties" "${PARTY_MAX:-25}"
set_conf "$PB" "AiPlayerbot.Party.MinLevel" "${PARTY_MIN_LEVEL:-15}"
set_conf "$PB" "AiPlayerbot.Party.LevelSpread" "${PARTY_LEVEL_SPREAD:-4}"
set_conf "$PB" "AiPlayerbot.Party.Teleport" "${PARTY_TELEPORT:-1}"
set_conf "$PB" "AiPlayerbot.Party.QueueLfg" "${PARTY_QUEUE_LFG:-1}"
set_conf "$PB" "AiPlayerbot.Party.TravelToDungeon" "${PARTY_TRAVEL:-1}"
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
set_conf "$PB" "AiPlayerbot.Llm.GreetOnLogin" "${LLM_GREET:-1}"
set_conf "$PB" "AiPlayerbot.Llm.GreetDelayMs" "${LLM_GREET_DELAY_MS:-15000}"
set_conf "$PB" "AiPlayerbot.Llm.EventsEnabled" "${LLM_EVENTS:-1}"
set_conf "$PB" "AiPlayerbot.Llm.EventChanceLevelUp" "${LLM_EVENT_LEVELUP:-100}"
set_conf "$PB" "AiPlayerbot.Llm.EventChanceDeath" "${LLM_EVENT_DEATH:-40}"
set_conf "$PB" "AiPlayerbot.Llm.EventChanceLoot" "${LLM_EVENT_LOOT:-35}"

log "ahbot"
AH=$CONF/modules/mod_ahbot.conf
set_conf "$AH" "AuctionHouseBot.EnableSeller" "1"
set_conf "$AH" "AuctionHouseBot.EnableBuyer" "1"
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

# MOTD moved out of worldserver.conf into acore_auth.motd; a Motd key in the config
# file is silently ignored by this core revision.
sed -i '/^Motd[[:space:]]*=/d' "$CONF/worldserver.conf"

log "realmlist -> ${REALM_ADDRESS}"
dc exec -T ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD" acore_auth -e \
  "UPDATE realmlist SET name='${REALM_NAME}', address='${REALM_ADDRESS}', localAddress='${REALM_ADDRESS}', port=${DOCKER_WORLD_EXTERNAL_PORT:-8085} WHERE id=1;
   INSERT INTO motd (realmid, text) VALUES (1, 'Welcome to ${REALM_NAME}.')
   ON DUPLICATE KEY UPDATE text=VALUES(text);"

log "configure complete - restart worldserver to apply"
