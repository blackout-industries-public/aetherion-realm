#!/usr/bin/env python3
"""Protect assembler-owned groups from upstream teardown, and optionally drive them.

Two verified defects:
1. LeaveOrDisbandGroup in ProcessBot(Player*) is reachable for grouped bots through
   RandomBotUpdateAction (the "random bot update" flag is sticky - set once while solo,
   never cleared), so upstream slowly tears assembler parties apart (~once per 16.7h
   per member at live config).
2. ProcessBot(uint32) parks grouped bots entirely, which also denies dead members the
   revive cycle - a wiped assembler party stayed dead until the sweeper found it.

The guard runs the revive cycle then stops before the destructive limbs (disband,
Randomize re-gear, Refresh - which ungroups - and RandomTeleportForLevel).
"""
import sys

path = sys.argv[1]
src = open(path).read()

if "PartyAssembler::Owns" in src:
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
GUARD_NEW = """    // An assembler-owned party must not be dismantled mid-run: everything below is
    // written for solo bots (disband, Randomize re-gear, Refresh - which ungroups -
    // and a random teleport). The revive cycle above is all an owned member needs.
    // This also closes the sticky-flag path through RandomBotUpdateAction that was
    // disbanding owned parties roughly once per member per 17 hours.
    if (Group* ownedGroup = bot->GetGroup())
        if (PartyAssembler::Owns(ownedGroup->GetGUID().GetCounter()))
            return false;

    // leave group if leader is rndbot
    Group* group = bot->GetGroup();"""
assert src.count(GUARD_ANCHOR) == 1, "ProcessBot(Player*) guard anchor"
src = src.replace(GUARD_ANCHOR, GUARD_NEW, 1)

open(path, "w").write(src)
print("patched RandomPlayerbotMgr.cpp (owned-group protection + optional drive)")
