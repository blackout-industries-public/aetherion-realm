#!/usr/bin/env python3
"""Economy BRD E3.1: repairs cost money.

Eight call sites repair random bots for free (cost=false); the dominant pair
fires on every death release, which made dying strictly cheaper than repairing.
Each site is gated on AiPlayerbot.Econ.PaidRepairs (default off): with it armed,
masterless bots keep their durability loss until they pay RepairAllAction at a
repair NPC - the vendor-visit arrival hook dispatches that. Master-owned bots
keep the free path (the operator's alts are not the economy).

RepairAllAction additionally gains the repair_paid emitter so spend is a
first-class economy event.
"""
import sys

module = sys.argv[1]

GATE = "NeedsLedger::PaidRepairs()"
FREE = "bot->DurabilityRepairAll(false, 1.0f, false);"


def patch(path, edits, marker):
    src = open(path).read()
    if marker in src:
        print(f"econ repairs already applied: {path}")
        return
    for anchor, replacement, count in edits:
        assert src.count(anchor) == count, \
            f"anchor count {src.count(anchor)} != {count} in {path}: {anchor[:50]!r}"
        src = src.replace(anchor, replacement)
    open(path, "w").write(src)
    print(f"patched {path}")


# Action files: botAI is in scope, so the operator's own alt bots stay exempt.
ACTION_GUARD = (
    f"    if (!{GATE} || IsRealPlayer(botAI->GetMaster()))\n"
    f"        {FREE}"
)
for rel, header, count in [
    ("src/Ai/Base/Actions/ReleaseSpiritAction.cpp", "ReleaseSpiritAction.h", 2),
    ("src/Ai/Base/Actions/TrainerAction.cpp", "TrainerAction.h", 2),
]:
    patch(f"{module}/{rel}", [
        (f'#include "{header}"\n',
         f'#include "{header}"\n#include "NeedsLedger.h"\n', 1),
        (f"    {FREE}", ACTION_GUARD, count),
    ], "NeedsLedger::PaidRepairs")

# UseMeetingStone's site sits deeper in a nested block.
patch(f"{module}/src/Ai/Base/Actions/UseMeetingStoneAction.cpp", [
    ('#include "UseMeetingStoneAction.h"\n',
     '#include "UseMeetingStoneAction.h"\n#include "NeedsLedger.h"\n', 1),
    (f"                    {FREE}",
     f"                    if (!{GATE} || IsRealPlayer(botAI->GetMaster()))\n"
     f"                        {FREE}", 1),
], "NeedsLedger::PaidRepairs")

# Factory and manager sites only ever run for random bots.
patch(f"{module}/src/Bot/Factory/PlayerbotFactory.cpp", [
    (f"    {FREE}",
     f"    if (!{GATE})\n        {FREE}", 2),
], "NeedsLedger::PaidRepairs")

# PartyAssembler.h is present from patch_processbot (2d), so it anchors the
# include; this patcher runs later in the chain.
patch(f"{module}/src/Bot/RandomPlayerbotMgr.cpp", [
    ('#include "PartyAssembler.h"\n',
     '#include "PartyAssembler.h"\n#include "NeedsLedger.h"\n', 1),
    (f"    {FREE}",
     f"    if (!{GATE})\n        {FREE}", 1),
], "NeedsLedger::PaidRepairs")

# The paid path's spend becomes an economy event, captured before the
# gold-cheat restore can distort the delta.
patch(f"{module}/src/Ai/Base/Actions/RepairAllAction.cpp", [
    ('#include "RepairAllAction.h"\n',
     '#include "RepairAllAction.h"\n#include "NeedsLedger.h"\n', 1),
    ("""        totalCost += bot->DurabilityRepairAll(true, discountMod, false);
""",
     """        totalCost += bot->DurabilityRepairAll(true, discountMod, false);

        // E8: what the wallet could not cover, the guild vault may. The core
        // enforces rank allowance and vault funds per item, so this second
        // pass repairs exactly what the guild will pay for and nothing else.
        if (bot->GetGuildId() && !botAI->HasCheat(BotCheatMask::gold))
            if (uint32 guildCost = bot->DurabilityRepairAll(true, discountMod, true))
                NeedsLedger::LogEvent("guild_repair", bot->GetGUID().GetCounter(), 0, 0,
                                      std::to_string(guildCost));
""", 1),
    ("""        if (totalCost > 0)
        {""",
     """        if (totalCost > 0 && !botAI->HasCheat(BotCheatMask::gold))
            NeedsLedger::LogEvent("repair_paid", bot->GetGUID().GetCounter(), 0, 0,
                                  std::to_string(totalCost));

        if (totalCost > 0)
        {""", 1),
], "NeedsLedger::PaidRepairs")
