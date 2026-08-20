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
CONF=conf/playerbots.conf.dist

git -C "$MODULE" checkout -- "$AI" "$SCRIPT" "$CONF"

mkdir -p "$MODULE/src/Ai/Llm" "$MODULE/src/Ai/Party"
cp "$HERE/src/Ai/Llm/LlmBridge.h" "$HERE/src/Ai/Llm/LlmBridge.cpp" "$MODULE/src/Ai/Llm/"
cp "$HERE/src/Ai/Party/PartyAssembler.h" "$HERE/src/Ai/Party/PartyAssembler.cpp" "$MODULE/src/Ai/Party/"

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
replacement = '''        if (!helper.ParseChatCommand(command, owner))
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

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootguid*/) override
    {
        // Only rare and better. Reacting to every grey drop would be both absurd and
        // ruinously expensive.
        if (!item || !item->GetTemplate() || item->GetTemplate()->Quality < ITEM_QUALITY_RARE)
            return;

        sLlmBridge->OnGameEvent(player, "loot",
            std::string(player->GetName()) + " just looted " + item->GetTemplate()->Name1);
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

open(path, "w").write(src)
print("patched Playerbots.cpp")
PY

# 2b. Bots forming their own parties (see patch_aifactory.py for why this is needed).
python3 "$HERE/patch_aifactory.py" "$MODULE/src/Bot/Factory/AiFactory.cpp"

# 2c. Party-size ambition, so groups get big enough to run dungeons.
python3 "$HERE/patch_grouper.py" "$MODULE/$AI"

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
AiPlayerbot.Party.MinLevel = 15
AiPlayerbot.Party.LevelSpread = 4
AiPlayerbot.Party.Teleport = 1
AiPlayerbot.Party.QueueLfg = 1
AiPlayerbot.Party.TravelToDungeon = 1

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
CONFEOF

echo "LLM patch applied to $MODULE"
