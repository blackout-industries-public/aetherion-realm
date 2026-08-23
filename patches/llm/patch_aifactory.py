"""Load the group strategy so bots can form their own parties.

Upstream ships `addStrategy("group")` commented out, which silently makes
AiPlayerbot.RandomBotGroupNearby a dead setting: InviteNearbyToGroupAction lives in
GroupStrategy, so the config gates an action no engine ever loads. That is why a
1500-bot realm contained exactly one group.

Only the random-bot site is enabled. The second site applies to bots owned by a real
player, where auto-inviting is deliberately suppressed anyway.
"""
import sys

path = sys.argv[1]
src = open(path).read()

ANCHOR = """            // nonCombatEngine->addStrategy("pvp", false);
            // nonCombatEngine->addStrategy("collision");
            // nonCombatEngine->addStrategy("group");
            // nonCombatEngine->addStrategy("guild");
            nonCombatEngine->addStrategy("grind", false);"""

REPLACEMENT = """            // World PvP. Upstream comments this out, so bots seek fights through the RPG
            // OutdoorPvp status but never engage enemy players properly once there.
            if (sConfigMgr->GetOption<bool>("AiPlayerbot.Pvp.Enabled", false))
                nonCombatEngine->addStrategy("pvp", false);
            // nonCombatEngine->addStrategy("collision");
            // Gated on the same setting that gates the invite action, so grouping can
            // be switched off at runtime without another rebuild.
            if (sPlayerbotAIConfig.randomBotGroupNearby)
                nonCombatEngine->addStrategy("group");
            // nonCombatEngine->addStrategy("guild");
            nonCombatEngine->addStrategy("grind", false);"""

if "AiPlayerbot.Pvp.Enabled" in src:
    print("AiFactory.cpp already patched")
    sys.exit(0)

assert ANCHOR in src, "AiFactory group/pvp anchor not found; upstream changed"
src = src.replace(ANCHOR, REPLACEMENT, 1)

if '#include "Config.h"' not in src:
    inc = '#include "AiFactory.h"'
    assert inc in src, "AiFactory include anchor not found"
    src = src.replace(inc, inc + '\n#include "Config.h"', 1)

open(path, "w").write(src)
print("patched AiFactory.cpp (group + world pvp strategies)")
