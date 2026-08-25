#!/usr/bin/env python3
"""Aim the far leg of an economic errand at the NPC it walked there for.

Measured defect. 478 bots held a bank errand and 16 of them had ever completed a
deposit, while guild vaults sat empty. The errand is issued correctly and the walk
is aimed correctly - 467 of those 478 genuinely carry depositable trade goods, and
only 11 have anything in a personal vault, so this is not a withdrawal errand
census. What fails is the arrival:

    GO_CAMP reaches the banker's position (within 10 yards)
      -> info.ChangeToWanderNpc()          with no target set
      -> NewRpgWanderNpcAction sees no target
      -> ChooseNpcOrGameObjectToInteract() -> urand over EVERY rpg target in 150y

In a capital that is a uniform draw over dozens of innkeepers, guards, vendors and
flightmasters, so the errand converts roughly one arrival in N and the bot wanders
off to somewhere it had no business being. The deposit only ever runs from the
WanderNpc first-reach block, which is why the same handful of bots did all the
banking - they were the ones who happened to be standing near a banker anyway and
took the near leg, which does set the target.

The near leg already picks the right NPC. This gives the far leg the same courtesy:
on arrival, if the bot holds an errand whose target is a creature, aim at the
nearest one that carries the matching flag. The random draw still decides when
nothing matches, so ordinary camping is unchanged.

errand_aim events record every arrival and whether anything matched, so the funnel
- verdict, arrival, deposit - can be read instead of inferred.

That funnel then showed half of all banker arrivals (83 of 165) doing nothing, and
concentrated: three bots accounted for 57 of them, one alone for 37. A bot that has
merely lost its bag between trigger and arrival does not do that thirty-seven times.
The candidate list an aim is drawn from rules out openly HOSTILE npcs only, while
GetNPCIfCanInteractWith - which the deposit must pass on arrival - also turns away
merely UNFRIENDLY ones, so a bot aimed at such a banker was refused at the counter
every single visit, forever. The aim now applies the same reputation test the
arrival will, and the arrival telemetry tells the two failures apart instead of
reporting both as "nothing to bank".
"""

import sys

path = sys.argv[1]
src = open(path).read()

if "errand_aim" in src:
    print("errand aim patch already applied")
    sys.exit(0)

assert '#include "NeedsLedger.h"' in src, (
    "NeedsLedger include (patch_econ_idle runs first)"
)

ANCHOR = """            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderNpc();
                return true;
            }
"""
NEW = """            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                // What this walk was for, asked before the variant is reassigned -
                // data and originalPos both reference it and are dead after the
                // change. Only creature errands: mailboxes, focus objects and nodes
                // are GameObjects and are aimed by their own branches.
                uint32 wantFlag = 0;
                switch (NeedsLedger::UrgentVerdict(bot->GetGUID().GetCounter()))
                {
                    case NeedsLedger::VERDICT_BANK:
                        wantFlag = UNIT_NPC_FLAG_BANKER;
                        break;
                    case NeedsLedger::VERDICT_AH:
                        wantFlag = UNIT_NPC_FLAG_AUCTIONEER;
                        break;
                    case NeedsLedger::VERDICT_TRAINER:
                        wantFlag = UNIT_NPC_FLAG_TRAINER;
                        break;
                    case NeedsLedger::VERDICT_VENDOR:
                        wantFlag = UNIT_NPC_FLAG_VENDOR;
                        break;
                    default:
                        break;
                }

                info.ChangeToWanderNpc();

                // Left untargeted, the arrival is handed to a uniform draw over
                // every rpg target within 150 yards. The bot walked a long way to
                // stand on this exact spot; the draw throws that away.
                if (wantFlag)
                {
                    ObjectGuid aim;
                    float bestAim = FLT_MAX;
                    GuidVector nearby =
                        context->GetValue<GuidVector>("possible new rpg targets")->Get();
                    for (ObjectGuid const& guid : nearby)
                    {
                        Creature* c = ObjectAccessor::GetCreature(*bot, guid);
                        if (!c || !c->HasNpcFlag(NPCFlags(wantFlag)))
                            continue;
                        // The candidate list only rules out openly hostile NPCs, but
                        // GetNPCIfCanInteractWith - which every one of these errands
                        // has to pass on arrival - also turns away merely unfriendly
                        // ones. Aiming at those produced bots that walked to the same
                        // banker over and over and were refused at the counter every
                        // time; one had done it thirty-seven times.
                        if (c->GetReactionTo(bot) <= REP_UNFRIENDLY)
                            continue;
                        float const away = bot->GetDistance(c);
                        if (away < bestAim)
                        {
                            bestAim = away;
                            aim = guid;
                        }
                    }
                    if (aim)
                        if (auto* aimed = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
                        {
                            aimed->npcOrGo = aim;
                            aimed->lastReach = 0;
                        }
                    NeedsLedger::LogEvent("errand_aim", bot->GetGUID().GetCounter(), wantFlag,
                                          aim ? 1 : 0, aim ? "aimed" : "none in range");
                }
                return true;
            }
"""
assert src.count(ANCHOR) == 1, "GO_CAMP arrival anchor"
src = src.replace(ANCHOR, NEW, 1)

# Standing at the banker and depositing are different events, and only the second
# one was recorded. Without the first, an errand that dies between arrival and
# deposit is indistinguishable from one that never arrived - which is the whole
# question. The action already gates itself on proximity, bag contents and the
# per-visit cap, so its answer is the interesting half.
REACH = """                if (c->HasNpcFlag(UNIT_NPC_FLAG_BANKER))
                    botAI->DoSpecificAction("bank deposit", Event(), true);
"""
REACH_NEW = """                if (c->HasNpcFlag(UNIT_NPC_FLAG_BANKER))
                {
                    // Two very different failures were being reported as one. The
                    // action re-resolves the banker through GetNPCIfCanInteractWith,
                    // which is stricter than the reach test that got us here - closer
                    // range, and no merely-unfriendly NPCs - so "the bot had nothing
                    // to do" and "the bot was never able to talk to this NPC at all"
                    // both logged as nothing to bank. The second is a wasted walk and
                    // has to be countable on its own.
                    bool const usable =
                        bot->GetNPCIfCanInteractWith(c->GetGUID(), UNIT_NPC_FLAG_BANKER) != nullptr;
                    bool const banked = botAI->DoSpecificAction("bank deposit", Event(), true);
                    NeedsLedger::LogEvent("errand_reach", bot->GetGUID().GetCounter(),
                                          UNIT_NPC_FLAG_BANKER, banked ? 1 : 0,
                                          banked ? "deposited"
                                                 : usable ? "nothing to bank"
                                                          : "banker not interactable");
                }
"""
assert src.count(REACH) == 1, "banker reach anchor (patch_econ_idle runs first)"
src = src.replace(REACH, REACH_NEW, 1)

open(path, "w").write(src)
print("patched NewRpgAction.cpp (errand far leg arrives aimed at its own npc)")
