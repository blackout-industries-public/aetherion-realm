#!/usr/bin/env bash
# Apply the Phase 2 LLM hook to the pinned mod-playerbots checkout.
#
# Restores the two touched files from git first, so this is idempotent and can
# also upgrade an older version of the patch in place.
set -euo pipefail

MODULE=${1:-/opt/warcraft/azerothcore/modules/mod-playerbots}
HERE=$(dirname "$(readlink -f "$0")")

[[ -d $MODULE/src ]] || { echo "not a mod-playerbots checkout: $MODULE" >&2; exit 1; }

AI=src/Bot/PlayerbotAI.cpp
SCRIPT=src/Script/Playerbots.cpp
FACTORY=src/Bot/Factory/AiFactory.cpp
CONF=conf/playerbots.conf.dist

# Every patched file is restored first so each patch always sees pristine anchors.
# AiFactory was missing from this list, so its patch matched a already-modified file
# and failed the second time it ran. The Economy BRD (E0.3) extended the list to
# every file the chain touches, making the whole apply deterministic; ownership map:
#   NewRpgAction.cpp        -> patch_questturnin (2c), future needs-preemption
#   RandomPlayerbotMgr.cpp  -> patch_processbot (2d), patch_eventlock (2f), patch_racemode (2g)
#   PlayerbotFactory.cpp    -> patch_racemode (2g), patch_econ (2h)
#   DestroyItemAction.cpp   -> patch_econ (2h)
NEWRPG=src/Ai/World/Rpg/Action/NewRpgAction.cpp
RNDMGR=src/Bot/RandomPlayerbotMgr.cpp
BOTFACTORY=src/Bot/Factory/PlayerbotFactory.cpp
DESTROYACT=src/Ai/Base/Actions/DestroyItemAction.cpp
SELLACT=src/Ai/Base/Actions/SellAction.cpp
ACTIONCTX=src/Ai/Base/ActionContext.h
RELEASEACT=src/Ai/Base/Actions/ReleaseSpiritAction.cpp
TRAINERACT=src/Ai/Base/Actions/TrainerAction.cpp
MEETSTONEACT=src/Ai/Base/Actions/UseMeetingStoneAction.cpp
GREETACT=src/Ai/Base/Actions/GreetAction.cpp
EMOTEACT=src/Ai/Base/Actions/EmoteAction.cpp
REPAIRACT=src/Ai/Base/Actions/RepairAllAction.cpp
AUTOMAINT=src/Ai/Base/Actions/AutoMaintenanceOnLevelupAction.cpp
git -C "$MODULE" checkout -- "$AI" "$SCRIPT" "$FACTORY" "$CONF" \
    "$NEWRPG" "$RNDMGR" "$BOTFACTORY" "$DESTROYACT" "$SELLACT" \
    "$RELEASEACT" "$TRAINERACT" "$MEETSTONEACT" "$REPAIRACT" "$ACTIONCTX" "$AUTOMAINT" "$GREETACT" "$EMOTEACT"

mkdir -p "$MODULE/src/Ai/Llm" "$MODULE/src/Ai/Party" "$MODULE/src/Ai/Econ"
cp "$HERE/src/Ai/Llm/LlmBridge.h" "$HERE/src/Ai/Llm/LlmBridge.cpp" "$MODULE/src/Ai/Llm/"
cp "$HERE/src/Ai/Party/PartyAssembler.h" "$HERE/src/Ai/Party/PartyAssembler.cpp" "$MODULE/src/Ai/Party/"
cp "$HERE/src/Ai/Econ/NeedsLedger.h" "$HERE/src/Ai/Econ/NeedsLedger.cpp" \
   "$HERE/src/Ai/Econ/AhSellAction.h" "$HERE/src/Ai/Econ/AhSellAction.cpp" \
   "$HERE/src/Ai/Econ/AhBuyAction.h" "$HERE/src/Ai/Econ/AhBuyAction.cpp" \
   "$HERE/src/Ai/Econ/MailCollectAction.h" "$HERE/src/Ai/Econ/MailCollectAction.cpp" \
   "$HERE/src/Ai/Econ/CraftPlanner.h" "$HERE/src/Ai/Econ/CraftPlanner.cpp" \
   "$HERE/src/Ai/Econ/EconCraftAction.h" "$HERE/src/Ai/Econ/EconCraftAction.cpp" \
   "$HERE/src/Ai/Econ/BankDepositAction.h" "$HERE/src/Ai/Econ/BankDepositAction.cpp" "$MODULE/src/Ai/Econ/"

# 1. Route chat the command parser did not understand to the bridge. That branch is
#    exactly the set of messages that are conversation rather than instructions.
python3 - "$MODULE/$AI" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()

anchor = '''        if (!helper.ParseChatCommand(command, owner) && it->GetType() == CHAT_MSG_WHISPER)
        {
            // ostringstream out; out << "Unknown command " << command;
            // TellPlayer(out);
            // helper.ParseChatCommand("help");
        }'''
replacement = '''        // Four or more words from a real player is conversation, not a
        // command: "pass me party leader" once parsed into an inventory dump
        // and an open trade window. Short forms stay commands; sentences go
        // to the model, whose intents route back through the same command
        // paths anyway.
        uint32 spacesInCommand = 0;
        for (char const ch : command)
            if (ch == ' ')
                ++spacesInCommand;
        bool const sentence = sLlmBridge->IsEnabled() && owner &&
                              !GET_PLAYERBOT_AI(owner) && spacesInCommand >= 3;
        if (sentence || !helper.ParseChatCommand(command, owner))
        {
            // Not a command, so treat it as conversation. One message is offered to
            // every bot that can hear it, so TryClaim picks a single responder;
            // without it a party message would produce one reply per bot.
            uint32 const chatType = it->GetType();
            if (sLlmBridge->IsEnabled() && sLlmBridge->WantsChatType(chatType) &&
                sLlmBridge->TryClaim(owner->GetGUID(), chatType, command))
            {
                sLlmBridge->Submit(bot, owner, command, chatType);
            }
        }'''
assert anchor in src, "unknown-command branch not found; upstream changed"
src = src.replace(anchor, replacement)

inc = '#include "PlayerbotAI.h"'
assert inc in src, "include anchor not found"
src = src.replace(inc, inc + '\n#include "LlmBridge.h"', 1)

# Every canned broadcast funnels through SayToChannel; noting the line is what
# lets a bot answer "me" to its own "who wants to team up?" with the thought
# still in hand.
say_note_anchor = '''bool PlayerbotAI::SayToChannel(const std::string& msg, const ChatChannelId& chanId)
{'''
assert say_note_anchor in src, "SayToChannel not found; upstream changed"
src = src.replace(say_note_anchor,
                  say_note_anchor + '\n    sLlmBridge->NoteBroadcast(bot, msg);\n', 1)

# "do it" parses as the 'do' command and every bot in the party answers
# "it: unknown action" in red. A real player's chat that names no known
# action is almost always conversation - one claimed bot answers like a
# person, the rest stay quiet.
unknown_anchor = '''    if (!silent)
    {
        out << name << ": unknown action";
        TellError(out.str());
    }

    return false;
}'''
unknown_new = '''    if (!silent)
    {
        Player* master = GetMaster();
        if (sLlmBridge->IsEnabled() && master && master->GetSession() &&
            !GET_PLAYERBOT_AI(master) && sLlmBridge->WantsChatType(CHAT_MSG_PARTY))
        {
            std::string const said = std::string(name);
            if (sLlmBridge->TryClaim(master->GetGUID(), CHAT_MSG_PARTY, said))
                sLlmBridge->Submit(bot, master, said, CHAT_MSG_PARTY);
            return false;
        }
        out << name << ": unknown action";
        TellError(out.str());
    }

    return false;
}'''
assert unknown_anchor in src, "unknown-action tail not found; upstream changed"
src = src.replace(unknown_anchor, unknown_new, 1)

open(path, "w").write(src)
print("patched PlayerbotAI.cpp")
PY

# 2. Drain on the world thread, load config at startup, and react in public channels.
python3 - "$MODULE/$SCRIPT" <<'PY'
import re, sys
path = sys.argv[1]
src = open(path).read()

m = re.search(r'^#include "[^"]+"', src, re.M)
assert m, "no include block found"
src = src[:m.start()] + '#include "Item.h"\n#include "ItemTemplate.h"\n#include "LlmBridge.h"\n#include "PartyAssembler.h"\n' + src[m.start():]

upd = '    void OnUpdate(uint32 diff) override\n    {\n'
assert upd in src, "WorldScript::OnUpdate not found; upstream changed"
src = src.replace(upd, upd + '        sLlmBridge->Drain(diff);\n        sPartyAssembler->Tick(diff);\n', 1)

init = '    void OnBeforeWorldInitialized() override\n    {\n'
assert init in src, "OnBeforeWorldInitialized not found; upstream changed"
src = src.replace(init, init + '        sLlmBridge->LoadConfig();\n        sPartyAssembler->LoadConfig();\n', 1)

# Public channels are handled here rather than through the command queue because the
# channel identity is only available at this call site, and because a channel can hold
# every bot on the realm - the reply must be one chosen bot, not all of them.

# Real world events and login greetings. Both are additive overrides on the existing
# PlayerScript; nothing that already worked changes behaviour.
login_anchor = """            PlayerbotsMgr::instance().AddPlayerbotData(player, false);
            sRandomPlayerbotMgr.OnPlayerLogin(player);"""
login_new = """            PlayerbotsMgr::instance().AddPlayerbotData(player, false);
            sRandomPlayerbotMgr.OnPlayerLogin(player);

            // Someone who already knows this player says hello, shortly after the
            // loading screen clears.
            sLlmBridge->OnHumanLogin(player);"""
assert login_anchor in src, "OnPlayerLogin body not found; upstream changed"
src = src.replace(login_anchor, login_new, 1)

# The script registers an explicit hook whitelist; an override the list does
# not name is never called. Every event override added below must enlist here,
# or it compiles fine and silently never fires - which is exactly how the
# levelup/death/loot events were dead from the day they were written.
hooks_anchor = """        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_BEFORE_TELEPORT
    }) {}"""
hooks_new = """        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_BEFORE_TELEPORT,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_PLAYER_JUST_DIED,
        PLAYERHOOK_ON_LOOT_ITEM,
        PLAYERHOOK_CAN_PLAYER_USE_CHAT
    }) {}"""
assert hooks_anchor in src, "PlayerScript hook whitelist not found; upstream changed"
src = src.replace(hooks_anchor, hooks_new, 1)

events_anchor = """    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* receiver) override"""
events_new = """    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        sLlmBridge->OnGameEvent(player, "levelup",
            std::string(player->GetName()) + " just reached level " +
            std::to_string(player->GetLevel()) + " (was " + std::to_string(oldLevel) + ")");
    }

    void OnPlayerJustDied(Player* player) override
    {
        sLlmBridge->OnGameEvent(player, "died", std::string(player->GetName()) + " just died");
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootguid) override
    {
        if (!item || !item->GetTemplate())
            return;
        ItemTemplate const* proto = item->GetTemplate();

        // Harvest ledger: every herb pulled, vein struck and hide skinned,
        // stamped with the zone it happened in. Skill-ups cannot carry the
        // professions heat map - they stop at cap - so the loot itself does.
        // Node produce arrives from a GameObject; skins from a creature.
        if (proto->Class == ITEM_CLASS_TRADE_GOODS &&
            ((lootguid.IsGameObject() && (proto->SubClass == ITEM_SUBCLASS_HERB ||
                                          proto->SubClass == ITEM_SUBCLASS_METAL_STONE)) ||
             proto->SubClass == ITEM_SUBCLASS_LEATHER))
            NeedsLedger::LogEvent("harvest", player->GetGUID().GetCounter(), proto->ItemId,
                                  count ? count : 1, std::to_string(player->GetZoneId()));

        // Only rare and better reaches the chat layer. Reacting to every grey
        // drop would be both absurd and ruinously expensive.
        if (proto->Quality < ITEM_QUALITY_RARE)
            return;

        sLlmBridge->OnGameEvent(player, "loot",
            std::string(player->GetName()) + " just looted " + proto->Name1);
    }

""" + events_anchor
assert events_anchor in src, "chat hook anchor not found; upstream changed"
src = src.replace(events_anchor, events_new, 1)

chan_anchor = '''        sRandomPlayerbotMgr.HandleCommand(type, msg, player);

        return true;'''
chan_new = '''        sRandomPlayerbotMgr.HandleCommand(type, msg, player);

        if (sLlmBridge->IsEnabled() && type == CHAT_MSG_CHANNEL && !player->GetSession()->IsBot())
        {
            uint32 const chanId = channel->GetChannelId();
            if ((chanId == ChatChannelId::GENERAL || chanId == ChatChannelId::TRADE ||
                 chanId == ChatChannelId::LOOKING_FOR_GROUP) &&
                sLlmBridge->RollChannelReply() &&
                sLlmBridge->TryClaim(player->GetGUID(), type, msg))
            {
                if (Player* responder = sLlmBridge->PickResponder(player))
                    sLlmBridge->Submit(responder, player, msg, CHAT_MSG_CHANNEL, chanId);
            }
        }

        return true;'''
assert chan_anchor in src, "channel hook anchor not found; upstream changed"
src = src.replace(chan_anchor, chan_new, 1)

# Open-world speech. The say/yell chat hook was never whitelisted, so bots
# could not hear a player standing right next to them - the operator's
# report was "not even a whisper to piss off". A real player speaking in
# earshot always gets exactly one answer: the ghosting was the bug, and a
# roll that stays silent 75% of the time is ghosting with extra steps. The
# reply-chance roll still governs bot-to-bot ambience.
say_anchor = """    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* receiver) override"""
say_new = """    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        if (type != CHAT_MSG_SAY && type != CHAT_MSG_YELL)
            return true;

        if (sLlmBridge->IsEnabled() && !player->GetSession()->IsBot() &&
            sLlmBridge->WantsChatType(type) &&
            sLlmBridge->TryClaim(player->GetGUID(), type, msg))
        {
            if (Player* responder = sLlmBridge->PickResponder(player))
                sLlmBridge->Submit(responder, player, msg, type);
        }

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* receiver) override"""
assert say_anchor in src, "whisper chat hook anchor not found; upstream changed"
src = src.replace(say_anchor, say_new, 1)

open(path, "w").write(src)
print("patched Playerbots.cpp")
PY

# 1b. Bots greet only real players. Left alone, every bot hellos every bot -
#     observed as an unbroken column of "greets everyone with a hearty hello!"
#     including one bot greeting itself.
python3 - "$MODULE/$GREETACT" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()

anchor = '''    Player* player = dynamic_cast<Player*>(botAI->GetUnit(guid));
    if (!player)
        return false;
'''
assert anchor in src, "GreetAction player lookup not found; upstream changed"
src = src.replace(anchor, anchor + '''
    // A hello is for people. Two thousand bots greeting each other is wall
    // spam, and the emote spends its charm where nobody reads it.
    if (!player->GetSession() || GET_PLAYERBOT_AI(player))
        return false;

    // Realm-wide trickle: greet memory resets on relogin, so every restart
    // made the whole crowd greet the player at once - a wall by other means.
    // One greet per 20s across all bots keeps it an occasional charm. The
    // load/store race across map threads is benign: at worst two greets land
    // in the same instant.
    static std::atomic<uint64> lastGreetMs{0};
    uint64 const nowMs = static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (nowMs - lastGreetMs.load(std::memory_order_relaxed) < 20000)
        return false;
    lastGreetMs.store(nowMs, std::memory_order_relaxed);
''', 1)

for header in ('#include <atomic>', '#include <chrono>'):
    if header not in src:
        src = src.replace('#include "Playerbots.h"',
                          header + '\n#include "Playerbots.h"', 1)

inc = '#include "Playerbots.h"'
assert inc in src, "Playerbots include not found in GreetAction"

open(path, "w").write(src)
print("patched GreetAction.cpp (greet real players only)")
PY

# 1c. Emote replies answer people, never bots. A received hello made the bot
#     hello back, which nearby bots received and returned - a self-sustaining
#     cascade that outlived the greet gate because it is a second emitter.
python3 - "$MODULE/src/Ai/Base/Actions/EmoteAction.cpp" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()

anchor = '''bool EmoteActionBase::ReceiveEmote(Player* source, uint32 emote, bool verbal)
{
'''
assert anchor in src, "ReceiveEmote head not found; upstream changed"
src = src.replace(anchor, anchor + '''    // Answering another bot's emote re-emits it to every bot in range - the
    // hello cascade. Emote replies are for people.
    if (!source || !source->GetSession() || GET_PLAYERBOT_AI(source))
        return false;

''', 1)

open(path, "w").write(src)
print("patched EmoteAction.cpp (emote replies answer people only)")
PY

# 2b. Bots forming their own parties (see patch_aifactory.py for why this is needed).
python3 "$HERE/patch_aifactory.py" "$MODULE/$FACTORY"

# 2c. Party-size ambition, so groups get big enough to run dungeons.
python3 "$HERE/patch_grouper.py" "$MODULE/$AI"

# 2c. Idle bots hand in finished quests (measured 247 accepts : 8 rewards before).
python3 "$HERE/patch_questturnin.py" "$MODULE/src/Ai/World/Rpg/Action/NewRpgAction.cpp"

# 2c2. Economy preemption at IDLE + vendor-visit selling (Economy BRD E2). Must
#      directly follow questturnin: it anchors on that patch's replacement text.
python3 "$HERE/patch_econ_idle.py" "$MODULE/src/Ai/World/Rpg/Action/NewRpgAction.cpp"

# 2d. Owned-group protection + optional revive drive (see patch_processbot.py header).
python3 "$HERE/patch_processbot.py" "$MODULE/src/Bot/RandomPlayerbotMgr.cpp"

# 2e. Steered travellers bypass the activity throttle (see patch_tripactive.py header).
python3 "$HERE/patch_tripactive.py" "$MODULE/src/Bot/PlayerbotAI.cpp"

# 2f. Event cache must be lock-guarded before MapUpdate.Threads goes above 1.
python3 "$HERE/patch_eventlock.py" "$MODULE/src/Bot/RandomPlayerbotMgr.cpp"

# 2g. Race mode: one-time factory init, bots keep only what they earn. Inert
#     unless AiPlayerbot.DisableRandomLevels = 1 (armed by RACE_MODE=1).
python3 "$HERE/patch_racemode.py" "$MODULE"

# 2h. Economy needs ledger + destruction emitters (Economy BRD E1). Observe-only
#     and gated on AiPlayerbot.Econ.*, all default off.
python3 "$HERE/patch_econ.py" "$MODULE"

# 2i. Paid repairs (Economy BRD E3.1): the eight free-repair sites gated, spend
#     emitted. Inert unless AiPlayerbot.Econ.PaidRepairs = 1.
python3 "$HERE/patch_econ_repair.py" "$MODULE"

# 2j. Paid training (Economy BRD E3.2): levelup/refresh free-teaching gated.
#     Inert unless AiPlayerbot.Econ.PaidTraining = 1.
python3 "$HERE/patch_econ_train.py" "$MODULE"

# 3. Config defaults. Everything off or conservative unless deliberately raised.
cat >> "$MODULE/$CONF" <<'CONFEOF'

###############################################################################
# Aetherion LLM bridge (Phase 2)
#
# Chat that the command parser does not recognise is forwarded to an external AI
# Bridge and the reply is spoken back on the same channel. Entirely optional:
# with Enabled = 0, or the bridge unreachable, bots behave exactly as before.
#
# Exactly one bot answers any given message. Public channels are additionally
# gated by ChannelReplyChance, because a channel can contain every bot on the
# realm and answering all of them would be both unreadable and ruinous.
###############################################################################

AiPlayerbot.Llm.Enabled = 0
AiPlayerbot.Llm.Host = "ai-bridge"
AiPlayerbot.Llm.Port = 8090
AiPlayerbot.Llm.TimeoutMs = 15000
AiPlayerbot.Llm.MaxInFlight = 8
AiPlayerbot.Llm.ClaimWindowMs = 10000

# Never spend inference on a line no human will receive. At 1500 bots almost all
# possible chatter has no audience, so this is the single largest saving available.
AiPlayerbot.Llm.RequireHumanWitness = 1
AiPlayerbot.Llm.SayRange = 45
AiPlayerbot.Llm.SameFactionOnly = 1

# Party-size ambition. Must total 100 with the implicit LEADER_5 remainder.
AiPlayerbot.Grouper.SoloPct = 10
AiPlayerbot.Grouper.MemberPct = 50
AiPlayerbot.Grouper.Leader2Pct = 5
AiPlayerbot.Grouper.Leader3Pct = 5
AiPlayerbot.Grouper.Leader4Pct = 10

# Background party assembly. Upstream only groups bots that can see each other, which
# on a large realm never produces a party big enough to run a dungeon.
AiPlayerbot.Party.Enabled = 0
AiPlayerbot.Party.IntervalMs = 60000
AiPlayerbot.Party.TargetSize = 5
AiPlayerbot.Party.MaxParties = 20
AiPlayerbot.Party.PerTick = 3
AiPlayerbot.Party.MinLevel = 15
AiPlayerbot.Party.LevelSpread = 4
AiPlayerbot.Party.Teleport = 1
AiPlayerbot.Party.SameMapOnly = 1
AiPlayerbot.Party.GatherRange = 400
AiPlayerbot.Party.ArriveRange = 60
AiPlayerbot.Party.MaxTripTicks = 40
AiPlayerbot.Party.StallTicks = 2
AiPlayerbot.Party.DriveGroupedBots = 0
AiPlayerbot.Party.InsideTicks = 20
AiPlayerbot.Party.SweepPerTick = 25
AiPlayerbot.Party.HuntRange = 8
AiPlayerbot.Party.NearestChoices = 4
AiPlayerbot.Party.FootRange = 1200
AiPlayerbot.Party.PortalPct = 50
AiPlayerbot.Party.RaidPct = 20
AiPlayerbot.Party.RaidSize = 10
AiPlayerbot.Party.Raid25Pct = 25
AiPlayerbot.Party.RaidHeroicPct = 15
AiPlayerbot.Party.MusterEveryMin = 45
AiPlayerbot.Party.MusterTimeoutMin = 12
AiPlayerbot.Party.WipeRetries = 3
AiPlayerbot.Party.QueueLfg = 1
AiPlayerbot.Party.TravelToDungeon = 1

# World PvP. The RPG OutdoorPvp status already sends bots looking for fights; this
# strategy is what makes them engage enemy players properly once they arrive.
AiPlayerbot.Pvp.Enabled = 0

AiPlayerbot.Llm.ReactWhisper = 1
AiPlayerbot.Llm.ReactParty = 1
AiPlayerbot.Llm.ReactGuild = 1
AiPlayerbot.Llm.ReactSay = 0
AiPlayerbot.Llm.ChannelReplyChance = 5
AiPlayerbot.Llm.SayReplyChance = 25

# Unprompted bot chatter in Trade. AmbientMaxDepth caps how many bots may answer
# one another before the thread stops; without a cap it would never stop.
AiPlayerbot.Llm.AmbientEnabled = 0
AiPlayerbot.Llm.AmbientIntervalMs = 300000
AiPlayerbot.Llm.AmbientMaxDepth = 1
AiPlayerbot.Llm.AmbientUseSay = 1

# A bot that already knows you says hello when you log in, and bots react to things
# that actually happen. Percentages keep the noisy events from dominating.
AiPlayerbot.Llm.GreetOnLogin = 1
AiPlayerbot.Llm.GreetDelayMs = 15000
AiPlayerbot.Llm.EventsEnabled = 1
AiPlayerbot.Llm.EventChanceLevelUp = 100
AiPlayerbot.Llm.EventChanceDeath = 40
AiPlayerbot.Llm.EventChanceLoot = 35

# Aetherion economy (Economy BRD). Observe-only needs ledger and event emitters;
# nothing changes bot behavior while these are the only keys on.
AiPlayerbot.Econ.Needs.Enabled = 0
AiPlayerbot.Econ.Needs.TickMs = 6000
AiPlayerbot.Econ.Needs.Shards = 10
AiPlayerbot.Econ.Events.Enabled = 0
AiPlayerbot.Econ.Preempt.Enabled = 0
AiPlayerbot.Econ.Vendor.Enabled = 0
AiPlayerbot.Econ.Vendor.FreeSlotsPct = 20
AiPlayerbot.Econ.Vendor.BrokeMinValue = 500
AiPlayerbot.Econ.Vendor.FarMaxYards = 3000
AiPlayerbot.Econ.ProtectTradeGoods = 0
AiPlayerbot.Econ.PaidRepairs = 0
AiPlayerbot.Econ.PaidTraining = 0
AiPlayerbot.Econ.Craft.Enabled = 0
AiPlayerbot.Econ.Bank.Enabled = 0
AiPlayerbot.Econ.Bank.MaxPerVisit = 6
AiPlayerbot.Econ.Ah.MinItemsForTrip = 3
AiPlayerbot.Econ.Ah.Enabled = 0
AiPlayerbot.Econ.Ah.MaxPerVisit = 4
AiPlayerbot.Econ.RemoteMail = 0
AiPlayerbot.Econ.Ah.Buy.Enabled = 0
AiPlayerbot.Econ.Mailbox.Enabled = 0
AiPlayerbot.Econ.Gather.Enabled = 0
AiPlayerbot.Econ.Gather.MaxTierGap = 150
AiPlayerbot.Econ.Duty.Scale = 100
CONFEOF

echo "LLM patch applied to $MODULE"
