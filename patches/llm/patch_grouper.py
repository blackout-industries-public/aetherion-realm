"""Make bot party-size ambition configurable.

Upstream hardcodes 20% SOLO / 60% MEMBER / 5% each of LEADER_2..LEADER_5. A LEADER_2
bot stops inviting at two members, so almost every self-formed party caps at two -
far too small to run a dungeon, which is why LFG never had anything to work with.

The thresholds become config so party sizes can be tuned without another rebuild.
Defaults are biased toward LEADER_4/LEADER_5 to produce dungeon-capable groups.
"""
import sys

path = sys.argv[1]
src = open(path).read()

ANCHOR = """GrouperType PlayerbotAI::GetGrouperType()
{
    uint32 grouperNumber = GetFixedBotNumber(100);

    if (grouperNumber < 20 && !HasGameClientMaster())
        return GrouperType::SOLO;

    if (grouperNumber < 80)
        return GrouperType::MEMBER;

    if (grouperNumber < 85)
        return GrouperType::LEADER_2;

    if (grouperNumber < 90)
        return GrouperType::LEADER_3;

    if (grouperNumber < 95)
        return GrouperType::LEADER_4;

    return GrouperType::LEADER_5;
}"""

REPLACEMENT = """GrouperType PlayerbotAI::GetGrouperType()
{
    // Stable per bot, so a character keeps the same grouping ambition for its life.
    uint32 grouperNumber = GetFixedBotNumber(100);

    // Percentages rather than upstream's hardcoded thresholds. The defaults here are
    // deliberately leader-heavy: a LEADER_2 bot stops inviting at two members, and a
    // realm full of two-bot parties can never field a dungeon group.
    uint32 const solo = sConfigMgr->GetOption<uint32>("AiPlayerbot.Grouper.SoloPct", 10);
    uint32 const member = sConfigMgr->GetOption<uint32>("AiPlayerbot.Grouper.MemberPct", 50);
    uint32 const leader2 = sConfigMgr->GetOption<uint32>("AiPlayerbot.Grouper.Leader2Pct", 5);
    uint32 const leader3 = sConfigMgr->GetOption<uint32>("AiPlayerbot.Grouper.Leader3Pct", 5);
    uint32 const leader4 = sConfigMgr->GetOption<uint32>("AiPlayerbot.Grouper.Leader4Pct", 10);

    if (grouperNumber < solo && !HasGameClientMaster())
        return GrouperType::SOLO;

    if (grouperNumber < solo + member)
        return GrouperType::MEMBER;

    if (grouperNumber < solo + member + leader2)
        return GrouperType::LEADER_2;

    if (grouperNumber < solo + member + leader2 + leader3)
        return GrouperType::LEADER_3;

    if (grouperNumber < solo + member + leader2 + leader3 + leader4)
        return GrouperType::LEADER_4;

    // Whatever percentage is left over aims for a full five-man.
    return GrouperType::LEADER_5;
}"""

if "AiPlayerbot.Grouper.SoloPct" in src:
    print("PlayerbotAI.cpp already has configurable grouper")
    sys.exit(0)

assert ANCHOR in src, "GetGrouperType anchor not found; upstream changed"
src = src.replace(ANCHOR, REPLACEMENT, 1)

if '#include "Config.h"' not in src:
    inc = '#include "PlayerbotAI.h"'
    assert inc in src, "include anchor not found"
    src = src.replace(inc, inc + '\n#include "Config.h"', 1)

open(path, "w").write(src)
print("patched PlayerbotAI.cpp (configurable grouper)")
