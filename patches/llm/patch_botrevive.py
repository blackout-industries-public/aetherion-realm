#!/usr/bin/env python3
"""Stop the random-bot manager from harvesting members out of a live dungeon run.

Measured defect. Party size decays with time spent inside - 5.00 at 0-9 ticks,
4.60 at 10-19, 4.33 at 20+ - and departed members turn up standing on map 571.
The path:

    ProcessBot(Player*)          dead branch, which runs BEFORE the ownership
                                 guard patch_processbot added
      -> Revive(player)
           -> Refresh(player)                 -> LeaveOrDisbandGroup()
           -> RandomTeleportGrindForLevel()   -> RandomTeleport -> Refresh again

So 60-300s (MinRandomBotReviveTime..Max) after any death, a member is removed from
the group and teleported to a random outdoor grind spot for its level - Northrend
for a level-80 wing, which is map 571. Long dungeons demand the longest stay, so
they collect the most deaths and lose the most members; the party reaches the first
boss two or three short and cannot kill it.

Two more entries reach the same Revive: FindCorpseAction at five deaths, and
ProcessBot(uint32) now that DriveGroupedBots is on.

Fixes:
1. Revive leaves the member of a party on a live run on its feet where it stands -
   no Refresh, no teleport. Where it goes next is the assembler's business.
2. The three unguarded LeaveOrDisbandGroup sites (RandomizeFirst, RandomizeMin,
   Refresh) refuse the same parties, closing them as an independent second route.

Both test OnRun, not Owns. Owns stays true for as long as an assembled group exists,
including for one whose journey ended without a disband - a door timeout, a lost
leader, a refused entry. Guarding on Owns would freeze those members out of the
maintenance cycle for the rest of the uptime; OnRun expires with the trip's own tick
budget, so a party that stops progressing is reclaimed exactly as it is today.

Every attempt is recorded through PartyAssembler::NoteRemoval, blocked or not, so
the guard reports what it stopped rather than the fix being taken on faith.
"""
import sys

path = sys.argv[1]
src = open(path).read()

if "PartyAssembler::NoteRemoval" in src:
    print("botrevive patch already applied")
    sys.exit(0)

assert '#include "PartyAssembler.h"' in src, "PartyAssembler include (patch_processbot runs first)"

REVIVE = """    SetEventValue(bot, "dead", 0, 0);
    SetEventValue(bot, "revive", 0, 0);

    Refresh(player);
    RandomTeleportGrindForLevel(player);
"""
REVIVE_NEW = """    SetEventValue(bot, "dead", 0, 0);
    SetEventValue(bot, "revive", 0, 0);

    // Reviving a member of a party on a live run used to end that run for them:
    // Refresh ungroups, and RandomTeleportGrindForLevel drops the bot at a random
    // outdoor grind spot for its level. Both fire within five minutes of any death,
    // so a run bled a member per death and arrived at its first boss short. On their
    // feet where they stand instead - the assembler owns where they go. Only while
    // the journey is live: a party that has stopped progressing still gets the full
    // maintenance cycle, which is the only thing that reclaims its members.
    if (Group* ownedGroup = player->GetGroup())
        if (PartyAssembler::OnRun(ownedGroup->GetGUID().GetCounter()))
        {
            PartyAssembler::NoteRemoval(player, "revive", true);
            if (!player->IsAlive())
            {
                player->ResurrectPlayer(1.0f);
                player->SpawnCorpseBones();
            }
            if (PlayerbotAI* reviveAI = GET_PLAYERBOT_AI(player))
            {
                // Same recovery the assembler's wipe path performs: combat
                // strategies rebuilt, then the queues that ResetStrategies just
                // restored taken straight back off.
                reviveAI->ResetStrategies(false);
                reviveAI->ChangeStrategy("-lfg,-bg", BOT_STATE_NON_COMBAT);
            }
            return;
        }

    Refresh(player);
    RandomTeleportGrindForLevel(player);
"""
assert src.count(REVIVE) == 1, "Revive body anchor"
src = src.replace(REVIVE, REVIVE_NEW, 1)

LEAVE = """    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();
"""


def guarded(site):
    return """    if (Group* ownedGroup = bot->GetGroup())
    {
        // A party on a live run is not this function's to dismantle. One whose
        // journey is over is: that is the only path that reclaims its members.
        bool const onRun = PartyAssembler::OnRun(ownedGroup->GetGUID().GetCounter());
        PartyAssembler::NoteRemoval(bot, "%s", onRun);
        if (!onRun)
            botAI->LeaveOrDisbandGroup();
    }
""" % site


# Identical text in three functions, so each is found from its own signature rather
# than by ordinal - the chain must not care what order upstream declares them in.
for signature, site in (
    ("void RandomPlayerbotMgr::RandomizeFirst(Player* bot)", "randomizefirst"),
    ("void RandomPlayerbotMgr::RandomizeMin(Player* bot)", "randomizemin"),
    ("void RandomPlayerbotMgr::Refresh(Player* bot)", "refresh"),
):
    start = src.find(signature)
    assert start != -1, f"{signature} not found"
    at = src.find(LEAVE, start)
    assert at != -1, f"LeaveOrDisbandGroup anchor in {signature}"
    src = src[:at] + guarded(site) + src[at + len(LEAVE):]

assert LEAVE not in src, "an unguarded LeaveOrDisbandGroup site remains"

open(path, "w").write(src)
print("patched RandomPlayerbotMgr.cpp (owned parties keep their members through revive)")
