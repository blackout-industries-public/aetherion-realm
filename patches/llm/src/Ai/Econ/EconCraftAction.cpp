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

// Two modes: the default picks castable-anywhere recipes for idle beats; the
// "focus" mode (dispatched on arrival at an anvil, forge or fire) picks only
// focus-required ones - the bot is standing at the object, and the core's
// CheckCast confirms the match.
bool PickCastable(Player* bot, CraftOption& out, bool focusMode)
{
    std::vector<CraftOption> options;
    CraftPlanner::Enumerate(bot, options, 12);
    for (CraftOption const& opt : options)
    {
        if (!opt.craftableNow)
            continue;
        if (focusMode != (opt.spellFocus != 0))
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
    return PickCastable(bot, opt, false) || PickCastable(bot, opt, true);
}

bool EconCraftAction::Execute(Event event)
{
    if (!CraftEnabled())
        return false;

    bool const focusMode = event.getParam() == "focus";
    CraftOption opt;
    if (!PickCastable(bot, opt, focusMode))
        return false;

    if (!botAI->CastSpell(opt.spellId, bot))
        return false;

    NeedsLedger::LogEvent("craft", bot->GetGUID().GetCounter(), opt.productItem,
                          opt.productCount, std::to_string(opt.spellId));
    return true;
}
