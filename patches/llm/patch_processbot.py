#!/usr/bin/env python3
"""Protect parties on a live run from upstream teardown, and optionally drive them.

Two verified defects:
1. LeaveOrDisbandGroup in ProcessBot(Player*) is reachable for grouped bots through
   RandomBotUpdateAction (the "random bot update" flag is sticky - set once while solo,
   never cleared), so upstream slowly tears assembler parties apart (~once per 16.7h
   per member at live config).
2. ProcessBot(uint32) parks grouped bots entirely, which also denies dead members the
   revive cycle - a wiped assembler party stayed dead until the sweeper found it.

The guard runs the revive cycle then stops before the destructive limbs (disband,
Randomize re-gear, Refresh - which ungroups - and RandomTeleportForLevel).

The gate at (2) tests Owns and the guard at (1) tests OnRun, deliberately. Owns is
"the assembler built this group" and stays true while the group exists; OnRun is "a
journey is underway" and expires with the trip's tick budget. So a party that has
stopped progressing still passes the gate and then gets the full maintenance cycle,
which is the only thing that ever reclaims its members - while a party mid-journey
is left alone. patch_botrevive (2d2) makes the revive cycle itself non-destructive
for the same live-run window.
"""
import sys

path = sys.argv[1]
src = open(path).read()

if "PartyAssembler::DriveGroupedBots" in src:
    print("processbot patch already applied")
    sys.exit(0)

INC = '#include "RandomPlayerbotMgr.h"\n'
assert src.count(INC) == 1, "include anchor"
src = src.replace(INC, INC + '#include "PartyAssembler.h"\n', 1)

GATE = """    if (player->GetGroup() || player->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;
"""
GATE_NEW = """    // Members of an assembler-owned party pass through when DriveGroupedBots is on,
    // solely so ProcessBot(Player*) can run their dead/revive cycle - its destructive
    // limbs stop at the ownership guard inside. Everyone else stays parked as before.
    bool const driveOwned = PartyAssembler::DriveGroupedBots() && player->GetGroup() &&
        PartyAssembler::Owns(player->GetGroup()->GetGUID().GetCounter());
    if ((player->GetGroup() && !driveOwned) || player->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;
"""
assert src.count(GATE) == 1, "ProcessBot(uint32) gate anchor"
src = src.replace(GATE, GATE_NEW, 1)

GUARD_ANCHOR = """    // leave group if leader is rndbot
    Group* group = bot->GetGroup();"""
GUARD_NEW = """    // A party on a live run must not be dismantled mid-journey: everything below is
    // written for solo bots (disband, Randomize re-gear, Refresh - which ungroups -
    // and a random teleport). This also closes the sticky-flag path through
    // RandomBotUpdateAction that was disbanding parties roughly once per member per
    // 17 hours. OnRun rather than Owns: an assembled group whose journey ended
    // without a disband - a door timeout, a lost leader, a party the trip check
    // refused - keeps its ownership entry for as long as the group exists, and
    // shielding those would freeze their members out of the maintenance cycle for
    // the rest of the uptime. A trip is bounded by its own tick budget, so the
    // shield lifts by itself the moment a party stops progressing.
    if (Group* ownedGroup = bot->GetGroup())
        if (PartyAssembler::OnRun(ownedGroup->GetGUID().GetCounter()))
            return false;

    // leave group if leader is rndbot
    Group* group = bot->GetGroup();"""
assert src.count(GUARD_ANCHOR) == 1, "ProcessBot(Player*) guard anchor"
src = src.replace(GUARD_ANCHOR, GUARD_NEW, 1)

open(path, "w").write(src)
print("patched RandomPlayerbotMgr.cpp (owned-group protection + optional drive)")
