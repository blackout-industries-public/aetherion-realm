/*
 * Aetherion economy: craft action (Economy BRD E7.1).
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#include "EconCraftAction.h"
#include "CraftPlanner.h"
#include "NeedsLedger.h"

#include "Config.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <string>
#include <vector>

namespace
{
bool CraftEnabled()
{
    static bool const enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Craft.Enabled", false);
    return enabled;
}

// V1 scope: castable-anywhere recipes. Focus-required ones (anvil, forge,
// alchemy lab) need the focus-object trip that comes with the next story.
bool PickCastable(Player* bot, CraftOption& out)
{
    std::vector<CraftOption> options;
    CraftPlanner::Enumerate(bot, options, 12);
    for (CraftOption const& opt : options)
    {
        if (!opt.craftableNow)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(opt.spellId);
        if (!info || info->RequiresSpellFocus)
            continue;
        out = opt;
        return true;
    }
    return false;
}
}  // namespace

bool EconCraftAction::isUseful()
{
    if (!CraftEnabled())
        return false;
    CraftOption opt;
    return PickCastable(bot, opt);
}

bool EconCraftAction::Execute(Event /*event*/)
{
    if (!CraftEnabled())
        return false;

    CraftOption opt;
    if (!PickCastable(bot, opt))
        return false;

    if (!botAI->CastSpell(opt.spellId, bot))
        return false;

    NeedsLedger::LogEvent("craft", bot->GetGUID().GetCounter(), opt.productItem,
                          opt.productCount, std::to_string(opt.spellId));
    return true;
}
