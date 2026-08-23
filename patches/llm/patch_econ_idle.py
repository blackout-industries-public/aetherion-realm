#!/usr/bin/env python3
"""Economy BRD E2.0/E2.1/E2.2: needs preemption at IDLE and the vendor visit.

Owns the same IDLE branch as patch_questturnin and MUST run directly after it -
the preemption anchor is questturnin's replacement text (BRD 3.4 ownership map).
Everything is gated on AiPlayerbot.Econ.* keys, default off: with them off the
verdict mirror is empty and both inserted blocks are dead branches.

The vendor trip reuses the existing WanderNpc status with a CHOSEN target, so no
enum surgery: preemption picks the nearest vendor-flagged possible target and the
first-reach branch sells greys and vendor-usage items (never auction-worthy
goods) through the real sell handler.
"""
import sys

path = sys.argv[1]
src = open(path).read()

MARKER = "NeedsLedger"
if MARKER in src:
    print("econ idle preemption already applied")
    sys.exit(0)

INC = '#include "NewRpgInfo.h"\n'
assert src.count(INC) == 1, "include anchor"
src = src.replace(INC, INC + '#include "AiObjectContext.h"\n#include "Creature.h"\n#include "GameObject.h"\n#include "NeedsLedger.h"\n', 1)

# E2.0/E2.1: the preemption block, second in line after the quest drain - an
# urgent economic errand beats a random pastime, a pending quest beats both.
IDLE_ANCHOR = """                info.ChangeToDoQuest(questId, quest);
                return true;
            }
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});"""
IDLE_NEW = """                info.ChangeToDoQuest(questId, quest);
                return true;
            }
            // E7.1: crafting is free time well spent - it consumes the idle
            // beat only when a recipe actually fires.
            if (NeedsLedger::CraftEnabled() &&
                botAI->DoSpecificAction("econ craft", Event(), true))
                return true;
            // Economy: an urgent errand beats a random pastime. The verdict
            // comes from the world-thread NeedsLedger mirror; trips reuse
            // WanderNpc with a deliberately chosen target, or GoCamp for the
            // far leg.
            uint8 const econVerdict = NeedsLedger::UrgentVerdict(bot->GetGUID().GetCounter());
            if (econVerdict == NeedsLedger::VERDICT_MAILBOX)
            {
                uint32 mbEntry, mbSpawn;
                float mx, my, mz;
                if (NeedsLedger::MailboxTarget(bot->GetGUID().GetCounter(), bot->GetMapId(),
                                               mbEntry, mbSpawn, mx, my, mz))
                {
                    if (bot->GetDistance(mx, my, mz) < 130.0f)
                    {
                        info.ChangeToWanderNpc();
                        if (auto* d = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
                        {
                            d->npcOrGo = ObjectGuid(HighGuid::GameObject, mbEntry, mbSpawn);
                            d->lastReach = 0;
                        }
                    }
                    else
                        info.ChangeToGoCamp(WorldPosition(bot->GetMapId(), mx, my, mz));
                    return true;
                }
            }
            // E7 focus trip: reagents ready, recipe needs an anvil/forge/fire.
            // The GO-target accessor serves both mailbox and focus verdicts.
            if (econVerdict == NeedsLedger::VERDICT_FOCUS)
            {
                uint32 fEntry, fSpawn;
                float fx, fy, fz;
                if (NeedsLedger::MailboxTarget(bot->GetGUID().GetCounter(), bot->GetMapId(),
                                               fEntry, fSpawn, fx, fy, fz))
                {
                    if (bot->GetDistance(fx, fy, fz) < 130.0f)
                    {
                        info.ChangeToWanderNpc();
                        if (auto* d = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
                        {
                            d->npcOrGo = ObjectGuid(HighGuid::GameObject, fEntry, fSpawn);
                            d->lastReach = 0;
                        }
                    }
                    else
                        info.ChangeToGoCamp(WorldPosition(bot->GetMapId(), fx, fy, fz));
                    return true;
                }
            }
            // E3.2: a funded training bill walks to the class trainer.
            if (econVerdict == NeedsLedger::VERDICT_TRAINER)
            {
                GuidVector possible = context->GetValue<GuidVector>("possible new rpg targets")->Get();
                ObjectGuid trainerGuid;
                float bestT = FLT_MAX;
                for (ObjectGuid const& guid : possible)
                {
                    Creature* c = ObjectAccessor::GetCreature(*bot, guid);
                    if (!c || !c->HasNpcFlag(UNIT_NPC_FLAG_TRAINER))
                        continue;
                    float d = bot->GetDistance(c);
                    if (d < bestT)
                    {
                        bestT = d;
                        trainerGuid = guid;
                    }
                }
                if (trainerGuid)
                {
                    info.ChangeToWanderNpc();
                    if (auto* d = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
                    {
                        d->npcOrGo = trainerGuid;
                        d->lastReach = 0;
                    }
                    return true;
                }
                float tx, ty, tz;
                if (NeedsLedger::FarVendor(bot->GetGUID().GetCounter(), bot->GetMapId(), tx, ty, tz))
                {
                    info.ChangeToGoCamp(WorldPosition(bot->GetMapId(), tx, ty, tz));
                    return true;
                }
            }
            // E4.2: deliberate auction-house trip; arrival handles sell+buy.
            if (econVerdict == NeedsLedger::VERDICT_AH)
            {
                GuidVector possible = context->GetValue<GuidVector>("possible new rpg targets")->Get();
                ObjectGuid auctioneer;
                float bestA = FLT_MAX;
                for (ObjectGuid const& guid : possible)
                {
                    Creature* c = ObjectAccessor::GetCreature(*bot, guid);
                    if (!c || !c->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER))
                        continue;
                    float d = bot->GetDistance(c);
                    if (d < bestA)
                    {
                        bestA = d;
                        auctioneer = guid;
                    }
                }
                if (auctioneer)
                {
                    info.ChangeToWanderNpc();
                    if (auto* d = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
                    {
                        d->npcOrGo = auctioneer;
                        d->lastReach = 0;
                    }
                    return true;
                }
                float ax, ay, az;
                if (NeedsLedger::FarVendor(bot->GetGUID().GetCounter(), bot->GetMapId(), ax, ay, az))
                {
                    info.ChangeToGoCamp(WorldPosition(bot->GetMapId(), ax, ay, az));
                    return true;
                }
            }
            if (econVerdict == NeedsLedger::VERDICT_VENDOR)
            {
                GuidVector possible = context->GetValue<GuidVector>("possible new rpg targets")->Get();
                ObjectGuid vendor;
                float best = FLT_MAX;
                for (ObjectGuid const& guid : possible)
                {
                    Creature* c = ObjectAccessor::GetCreature(*bot, guid);
                    if (!c || !c->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
                        continue;
                    float d = bot->GetDistance(c);
                    if (d < best)
                    {
                        best = d;
                        vendor = guid;
                    }
                }
                if (vendor)
                {
                    info.ChangeToWanderNpc();
                    if (auto* d = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
                    {
                        d->npcOrGo = vendor;
                        d->lastReach = 0;
                    }
                    return true;
                }
                // E2.1b far leg: nothing in scan range - walk toward the
                // nearest vendor spawn; the next IDLE there finds it nearby.
                float vx, vy, vz;
                if (NeedsLedger::FarVendor(bot->GetGUID().GetCounter(), bot->GetMapId(), vx, vy, vz))
                {
                    info.ChangeToGoCamp(WorldPosition(bot->GetMapId(), vx, vy, vz));
                    return true;
                }
            }
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});"""
assert src.count(IDLE_ANCHOR) == 1, "idle anchor (run after patch_questturnin)"
src = src.replace(IDLE_ANCHOR, IDLE_NEW, 1)

# E2.2: every arrival at a vendor is a selling moment - greys and vendor-usage
# only, so auction-worthy goods keep waiting for the AH epics.
REACH_ANCHOR = """        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);
            return true;
        }"""
REACH_NEW = """        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);
            if (Creature* c = object->ToCreature())
            {
                if (NeedsLedger::SellOnVendorVisit() && c->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
                    botAI->DoSpecificAction("sell", Event("rpg action", "trash"), true);
                // E3.1: the same visit settles the repair bill when the NPC
                // can take it - budget checks live inside the core charge path.
                if (NeedsLedger::PaidRepairs() && c->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
                    botAI->DoSpecificAction("repair", Event(), true);
                // E4.1a: auctioneers are already wander targets; every arrival
                // is a listing moment. The action re-validates proximity and
                // gates itself on Econ.Ah.Enabled.
                if (c->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER))
                {
                    botAI->DoSpecificAction("ah sell", Event(), true);
                    // E8.2: the same visit shops for upgrades from the mirror.
                    botAI->DoSpecificAction("ah buy", Event(), true);
                }
                // E3.2: TrainerAction reads the bot's selection as its target.
                if (NeedsLedger::PaidTraining() && c->HasNpcFlag(UNIT_NPC_FLAG_TRAINER))
                {
                    bot->SetSelection(c->GetGUID());
                    botAI->DoSpecificAction("trainer", Event("trainer", "learn"), true);
                }
                // E6.3a: bankers take the surplus mats a crafter's recipes
                // do not consume.
                if (c->HasNpcFlag(UNIT_NPC_FLAG_BANKER))
                    botAI->DoSpecificAction("bank deposit", Event(), true);
            }
            // E5.2: arriving at a mailbox is the collection moment; E7: an
            // anvil/forge/fire arrival is the crafting moment.
            if (GameObject* go = object->ToGameObject())
            {
                if (go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX)
                    botAI->DoSpecificAction("mail collect", Event(), true);
                if (go->GetGoType() == GAMEOBJECT_TYPE_SPELL_FOCUS)
                    botAI->DoSpecificAction("econ craft", Event("econ craft", "focus"), true);
            }
            return true;
        }"""
assert src.count(REACH_ANCHOR) == 1, "wander-npc first-reach anchor"
src = src.replace(REACH_ANCHOR, REACH_NEW, 1)

open(path, "w").write(src)
print("patched NewRpgAction.cpp (econ idle preemption + vendor-visit sell)")
