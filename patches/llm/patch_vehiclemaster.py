#!/usr/bin/env python3
"""Vehicle tactics answer to the group leader when there is no master.

Every vehicle encounter the module implements - Flame Leviathan's siege yard, the
Oculus drakes, Malygos's phase three, the Putricide abomination, the Halion adds -
was written against a human master: the trigger that mounts a bot asks whether
`botAI->GetMaster()` is already sitting in a vehicle, and the drive and formation
actions follow the master's vehicle around.

In a raid of bots there is no master. PlayerbotAI::FindNewMaster() returns the
group leader only when the leader is a real player and otherwise hunts for a real
player in the group, so a bot-only group leaves `master` null and every one of
those triggers returns false forever. That is why the assembler's Ulduar bootstrap
seated a leader and nothing followed: there was nobody for the followers to be
following.

The fix is deliberately narrow. A new accessor falls back to the group leader, and
ONLY the vehicle files are switched to it - the general GetMaster() keeps its
meaning everywhere else (chat, loot rules, "is a real player watching" checks),
because making every random bot treat its leader as a master would change far
more than boarding. The leader itself resolves to itself, which is what lets the
assembler seat it and have the rest of the raid mount behind it.
"""
import sys
from pathlib import Path

module = Path(sys.argv[1])

# ---- 1. the accessor ---------------------------------------------------------
header = module / "src/Bot/PlayerbotAI.h"
src = header.read_text()
anchor = "    Player* GetMaster() { return master; }\n"
marker = "GetMasterOrLeader"
assert anchor in src, "GetMaster accessor not found; upstream changed"
if marker not in src:
    src = src.replace(anchor, anchor + (
        "    // Vehicle and formation tactics were written against a human master. A\n"
        "    // raid of bots has none, so those - and only those - fall back to the\n"
        "    // group leader, the character the party assembler seats first.\n"
        "    Player* GetMasterOrLeader() { return master ? master : GetGroupLeader(); }\n"
    ), 1)
    header.write_text(src)
print("patched PlayerbotAI.h (GetMasterOrLeader)")

# ---- 2. the vehicle files ----------------------------------------------------
# path -> the number of accessor calls the pristine file holds. Counted rather than
# assumed so a silent upstream change fails loudly here instead of at runtime.
files = {
    "src/Ai/Raid/Uld/UldTriggers.cpp": 5,
    "src/Ai/Raid/Uld/UldActions.cpp": 7,
    "src/Ai/Dungeon/OC/OCTriggers.cpp": 3,
    "src/Ai/Dungeon/OC/OCActions.cpp": 2,
    "src/Ai/Dungeon/OC/OCMultipliers.cpp": 1,
    "src/Ai/Raid/EoE/EoEActions.cpp": 1,
    "src/Ai/Raid/ICC/Action/ICCActions_PP.cpp": 2,
    "src/Ai/Raid/RS/Action/RSActions_ADD.cpp": 1,
}
old = "botAI->GetMaster()"
new = "botAI->GetMasterOrLeader()"
for rel, expected in files.items():
    path = module / rel
    src = path.read_text()
    if new in src:
        print(f"already patched {rel}")
        continue
    found = src.count(old)
    assert found == expected, f"{rel}: expected {expected} master calls, found {found}; upstream changed"
    path.write_text(src.replace(old, new))
    print(f"patched {rel} ({found} calls now answer to the leader)")
