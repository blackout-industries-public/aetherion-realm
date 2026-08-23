#!/usr/bin/env python3
"""Economy BRD E3.2: class training costs money.

Two free-teaching sites made trainers pointless: every levelup and every factory
Refresh silently granted all class spells. Both are gated on
AiPlayerbot.Econ.PaidTraining (default off); with it armed, masterless bots keep
a growing training bill in the needs ledger, and the trainer-visit errand pays
it through TrainerAction's real charge path. Creation-time teaching stays free -
a fresh bot must know its starting kit.
"""
import sys

module = sys.argv[1]

GATE = "NeedsLedger::PaidTraining()"


def patch(path, edits, marker):
    src = open(path).read()
    if marker in src:
        print(f"econ training already applied: {path}")
        return
    for anchor, replacement, count in edits:
        assert src.count(anchor) == count, \
            f"anchor count {src.count(anchor)} != {count} in {path}: {anchor[:50]!r}"
        src = src.replace(anchor, replacement)
    open(path, "w").write(src)
    print(f"patched {path}")


patch(f"{module}/src/Ai/Base/Actions/AutoMaintenanceOnLevelupAction.cpp", [
    ('#include "AutoMaintenanceOnLevelupAction.h"\n',
     '#include "AutoMaintenanceOnLevelupAction.h"\n#include "NeedsLedger.h"\n', 1),
    ("""    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitSkills();
    factory.InitClassSpells();
    factory.InitAvailableSpells();
    factory.InitPet();""",
     """    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitSkills();
    if (!NeedsLedger::PaidTraining())
    {
        factory.InitClassSpells();
        factory.InitAvailableSpells();
    }
    factory.InitPet();""", 1),
], "NeedsLedger::PaidTraining")

# The Refresh pair (PlayerbotFactory.cpp ~:908) re-teaches on every factory
# refresh; the creation-time pairs earlier in Randomize stay untouched. The
# anchor is the two adjacent calls unique to Refresh's body.
patch(f"{module}/src/Bot/Factory/PlayerbotFactory.cpp", [
    ("""    InitClassSpells();
    InitAvailableSpells();
    InitReputation();""",
     """    if (!NeedsLedger::PaidTraining())
    {
        InitClassSpells();
        InitAvailableSpells();
    }
    InitReputation();""", 1),
], "NeedsLedger::PaidTraining")
