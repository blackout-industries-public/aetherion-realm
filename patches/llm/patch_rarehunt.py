#!/usr/bin/env python3
"""Economy BRD E10: the rare hunt's two legs at IDLE, and the kill on arrival.

The hunter persona's verdict is VERDICT_RARE, issued last in the NeedsLedger
chain so no economic errand ever loses a beat to a trophy. This turns that
verdict into travel using exactly the machinery the gather errand already uses
in production - aim the wander at the live creature when it is close enough to
resolve, otherwise walk to its spawn point and let the next idle beat there do
the aiming - so there is no second travel system to keep honest.

Three things are deliberate:

  * Liveness is checked twice. The world thread already refused any spawn with a
    pending respawn time, which is the only test that works for a grid nobody has
    loaded. This adds the test that only works when the grid IS loaded: resolve
    the creature through the map's spawn-id store and ask whether it is alive.

  * A target that fails the near test is RETIRED, not re-rolled. A bot standing
    at an empty spawn point that keeps re-claiming the same verdict is the exact
    shape of the bug that ran dungeon parties into the ground for 600 runs.
    RetireRare drops the held verdict, the bot rolls an ordinary pastime, and
    the next ledger pass picks something that is actually up.

  * The arrival attack is a fallback, not the mechanism. A hostile rare aggroes
    on approach and combat starts long before the bot is ever "reached". What it
    covers is the rare far enough below the bot to ignore it, which would leave
    a hunter standing on top of its own target forever.

Owns the same NewRpgAction.cpp as patch_questturnin (2c), patch_econ_idle (2c2)
and patch_errandaim (2c3), and MUST run after 2c2 - both of its anchors are text
that patch_econ_idle wrote.
"""

import sys

path = sys.argv[1]
src = open(path).read()

if "VERDICT_RARE" in src:
    print("rare hunt patch already applied")
    sys.exit(0)

assert '#include "NeedsLedger.h"' in src, "NeedsLedger include (patch_econ_idle runs first)"

# The hunt sits at the very end of the preemption chain, immediately before the
# ordinary pastime roll: everything above it keeps a bot solvent or supplied.
IDLE_ANCHOR = """            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});"""
IDLE_NEW = """            // E10 rare hunt: the hunter persona walks to a named rare the ledger
            // already sized against its level. Same two legs as every other
            // errand, and the same 130-yard split between them.
            if (econVerdict == NeedsLedger::VERDICT_RARE)
            {
                uint32 rEntry, rSpawn;
                float rx, ry, rz;
                if (NeedsLedger::RareTarget(bot->GetGUID().GetCounter(), bot->GetMapId(),
                                            rEntry, rSpawn, rx, ry, rz))
                {
                    // DB spawns carry a map-generated lowguid, so a guid built
                    // from spawnId resolves nothing; find the live object. The
                    // entry check matters because the store is keyed on spawn id
                    // alone and a multispawn can seat a different creature there.
                    Creature* rare = nullptr;
                    {
                        auto bounds =
                            bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(rSpawn);
                        for (auto it = bounds.first; it != bounds.second; ++it)
                            if (it->second && it->second->GetEntry() == rEntry)
                            {
                                rare = it->second;
                                break;
                            }
                    }
                    bool const nearSpawn = bot->GetDistance(rx, ry, rz) < 130.0f;
                    if (rare && rare->IsAlive() && nearSpawn)
                    {
                        info.ChangeToWanderNpc();
                        if (auto* d = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
                        {
                            d->npcOrGo = rare->GetGUID();
                            d->lastReach = 0;
                        }
                        return true;
                    }
                    if (nearSpawn)
                    {
                        // Standing at the spawn point with nothing to hunt.
                        // Retiring the verdict is the whole point: a target the
                        // bot cannot engage has to stop being claimed, or the
                        // bot ping-pongs here until the rare respawns.
                        NeedsLedger::LogEvent("rare_hunt", bot->GetGUID().GetCounter(), rEntry, 0,
                                              "gone|" + std::to_string(bot->GetMapId()) + "|" +
                                                  (rare ? "dead" : "absent"));
                        NeedsLedger::RetireRare(bot->GetGUID().GetCounter());
                    }
                    else
                    {
                        info.ChangeToGoCamp(WorldPosition(bot->GetMapId(), rx, ry, rz));
                        return true;
                    }
                }
            }
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});"""
assert src.count(IDLE_ANCHOR) == 1, "idle pastime-roll anchor (run after patch_econ_idle)"
src = src.replace(IDLE_ANCHOR, IDLE_NEW, 1)

# Arriving beside the rare. The action re-resolves the target from the mirror by
# spawn id, so a bot that merely wandered onto some other rare cannot start a
# fight nothing ever sized for it.
REACH = """                // E6.3a: bankers take the surplus mats a crafter's recipes
                // do not consume.
"""
REACH_NEW = """                // E10: the hunt's closing move, for the rare that never
                // noticed the bot coming. Guarded on rank so the mirror is
                // only consulted when something rare is actually standing
                // here; the action itself checks that it is THIS bot's target.
                if (NeedsLedger::RareHuntEnabled() &&
                    (c->GetCreatureTemplate()->rank == CREATURE_ELITE_RARE ||
                     c->GetCreatureTemplate()->rank == CREATURE_ELITE_RAREELITE))
                    botAI->DoSpecificAction("rare hunt", Event(), true);
                // E6.3a: bankers take the surplus mats a crafter's recipes
                // do not consume.
"""
assert src.count(REACH) == 1, "banker reach comment anchor (patch_econ_idle runs first)"
src = src.replace(REACH, REACH_NEW, 1)

# The far leg lands the bot within ten yards of the rare's spawn point and then
# hands the arrival to WANDER_NPC. patch_errandaim gives that arrival an aim for
# every errand whose target carries an NPC flag - a rare carries none, so it fell
# through to the uniform draw over every rpg target within 150 yards and the bot
# walked away from the thing it had just crossed a zone to reach. Measured: 110
# aims, 38 hunts held, not one arrival in half an hour; the closest bot closed 29
# yards in 150 seconds while alternating between two targets.
#
# Anchors on patch_errandaim's own replacement text and must run after it. The aim
# goes in before that patch's wantFlag block, which is inert for a rare verdict,
# so neither overwrites the other.
ARRIVE = """                info.ChangeToWanderNpc();

                // Left untargeted, the arrival is handed to a uniform draw over
"""
ARRIVE_NEW = """                info.ChangeToWanderNpc();

                // E10: a rare has no NPC flag to aim by, so the flag-driven block
                // below cannot claim this arrival and the uniform draw would walk
                // the bot away from the spawn it just crossed a zone for. Aim by
                // DB spawn id instead - the one identity a hunt actually has.
                if (NeedsLedger::RareHuntEnabled())
                {
                    uint32 aEntry, aSpawn;
                    float ax, ay, az;
                    if (NeedsLedger::RareTarget(bot->GetGUID().GetCounter(), bot->GetMapId(),
                                                aEntry, aSpawn, ax, ay, az) &&
                        // This block runs on EVERY completed camp trip, and a
                        // hunter takes ordinary camp trips too. Without this the
                        // arrival of an unrelated walk was read as the hunt's
                        // arrival and retired a perfectly good target - caught in
                        // production as 'gone|571|2127y arrival', a bot reporting
                        // it had arrived somewhere over a mile from its prey.
                        bot->GetDistance(ax, ay, az) < 60.0f)
                    {
                        Creature* prey = nullptr;
                        auto bounds =
                            bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(aSpawn);
                        for (auto pit = bounds.first; pit != bounds.second; ++pit)
                            if (pit->second && pit->second->GetEntry() == aEntry)
                            {
                                prey = pit->second;
                                break;
                            }
                        if (prey && prey->IsAlive())
                            if (auto* aimed = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
                            {
                                aimed->npcOrGo = prey->GetGUID();
                                aimed->lastReach = 0;
                            }
                        bool const standing = prey && prey->IsAlive();
                        // Third field carries how close the far leg actually put
                        // the bot, so a travel failure and a combat failure are
                        // different rows instead of the same silence.
                        NeedsLedger::LogEvent(
                            "rare_hunt", bot->GetGUID().GetCounter(), aEntry, standing ? 1 : 0,
                            std::string(standing ? "reach|" : "gone|") +
                                std::to_string(bot->GetMapId()) + "|" +
                                std::to_string(uint32(bot->GetDistance(ax, ay, az))) +
                                (standing ? "y" : "y arrival"));
                        // Walked the whole way and there is nothing here. Same
                        // rule as the near leg: retire it rather than let the
                        // next pass hand back the same empty spawn point.
                        if (!standing)
                            NeedsLedger::RetireRare(bot->GetGUID().GetCounter());
                        return true;
                    }
                }

                // Left untargeted, the arrival is handed to a uniform draw over
"""
assert src.count(ARRIVE) == 1, "GO_CAMP arrival aim anchor (patch_errandaim runs first)"
src = src.replace(ARRIVE, ARRIVE_NEW, 1)

open(path, "w").write(src)
print("patched NewRpgAction.cpp (rare hunt travel + arrival kill)")
