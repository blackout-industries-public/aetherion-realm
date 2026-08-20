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

REPLACEMENT = """            // nonCombatEngine->addStrategy("pvp", false);
            // nonCombatEngine->addStrategy("collision");
            // Gated on the same setting that gates the invite action, so grouping can
            // be switched off at runtime without another rebuild.
            if (sPlayerbotAIConfig.randomBotGroupNearby)
                nonCombatEngine->addStrategy("group");
            // nonCombatEngine->addStrategy("guild");
            nonCombatEngine->addStrategy("grind", false);"""

if "randomBotGroupNearby)" in src and 'addStrategy("group")' in src:
    print("AiFactory.cpp already patched")
    sys.exit(0)

assert ANCHOR in src, "AiFactory group-strategy anchor not found; upstream changed"
open(path, "w").write(src.replace(ANCHOR, REPLACEMENT, 1))
print("patched AiFactory.cpp (group strategy)")
