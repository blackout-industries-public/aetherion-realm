#!/usr/bin/env python3
"""Exempt actively-steered trip participants from the activity throttle.

A throttled leader cannot outrun the stall detector: at a 10-20% duty cycle the
90-second no-progress teleport fires long before a walk completes - measured 92% of
trips falling back to it even after the SmartScale band was widened. Instances and
battlegrounds are already forced active upstream (the isOverworld check); this adds the
missing overworld travel leg, scoped to the handful of characters the assembler is
steering, gated on the same DriveGroupedBots toggle.
"""
import sys

path = sys.argv[1]
src = open(path).read()

if "PartyAssembler::IsDriven" in src:
    print("trip-active patch already applied")
    sys.exit(0)

ANCHOR = """    // bot is waiting in a BG queue — stay active to speed up join
    if (bot->InBattlegroundQueue())
        return true;
"""
assert src.count(ANCHOR) == 1, "BG-queue anchor missing or ambiguous"
src = src.replace(ANCHOR, ANCHOR + """
    // bot is being steered along an assembler journey - the trip leader on the road,
    // or a party being summoned at the door. Bounded by MaxParties, typically a few
    // dozen characters.
    if (PartyAssembler::DriveGroupedBots() &&
        PartyAssembler::IsDriven(bot->GetGUID().GetCounter()))
        return true;
""", 1)

INC = '#include "PlayerbotAI.h"\n'
if '#include "PartyAssembler.h"' not in src:
    assert src.count(INC) == 1, "include anchor"
    src = src.replace(INC, INC + '#include "PartyAssembler.h"\n', 1)

open(path, "w").write(src)
print("patched PlayerbotAI.cpp (trip participants exempt from throttle)")
