#!/usr/bin/env python3
"""Stop RPG movement from walking a bot out of a fight.

The RPG subtree has no combat guard anywhere - `grep -rn IsInCombat src/Ai/World/Rpg/`
returns nothing - and the engine choice that appears to provide one is a latch rather
than a function of combat state. BOT_STATE_COMBAT is entered only by AttackAction and
PullActions; it is LEFT by DropTargetAction the moment the current target becomes
invalid. So when the first mob of a pack dies, the bot drops back to the non-combat
engine while still very much in combat, and the queue falls through 'dps assist'
(which needs a resolvable dps target) and 'attack anything' (hard-disabled in combat)
to 'new rpg go grind', which walks it away from the surviving mobs. Worse, MoveFarTo
teleports the bot to its destination after 90 seconds without progress - mid-fight.

Measured consequence on this realm: dungeon parties netted about 22 yards per
45-second tick against a 500-yard wing, 36 Forge of Souls instances produced zero
boss kills, and log_encounter recorded no five-man encounters at all in 24 hours.
Members follow the party leader, so a leader that strolls off takes the party with it
and the pack resets behind them.

The guard goes on the base action so every RPG movement inherits it: grind, camp,
wander, quest and flight all have no business running while the bot is being hit.
The RPG destination itself is left untouched, so the moment the fight ends the same
action becomes useful again and the bot carries on where it left off - no re-issue and
no dead time.
"""
import sys

module = sys.argv[1]
header = f"{module}/src/Ai/World/Rpg/Action/NewRpgBaseAction.h"
source = f"{module}/src/Ai/World/Rpg/Action/NewRpgBaseAction.cpp"

src = open(header).read()
if "isUseful" in src:
    print("rpg-combat patch already applied")
    sys.exit(0)

ANCHOR = """    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}
"""
assert src.count(ANCHOR) == 1, "NewRpgBaseAction constructor anchor missing or ambiguous"
src = src.replace(
    ANCHOR,
    ANCHOR + """
    // Never while fighting. The engine latch releases mid-pack, and everything below
    // this class moves the bot somewhere else when it does.
    bool isUseful() override;
""",
    1,
)
open(header, "w").write(src)

body = open(source).read()
assert "bool NewRpgBaseAction::isUseful" not in body, "isUseful already defined"

INC = '#include "NewRpgBaseAction.h"\n'
assert body.count(INC) == 1, "NewRpgBaseAction include anchor missing or ambiguous"
body = body.replace(
    INC,
    INC + """
bool NewRpgBaseAction::isUseful()
{
    // The destination survives the fight: rpgInfo is not cleared here, so the walk
    // resumes by itself on the first tick after combat ends.
    return !bot->IsInCombat();
}
""",
    1,
)
open(source, "w").write(body)
print("patched NewRpgBaseAction (rpg movement holds while in combat)")
