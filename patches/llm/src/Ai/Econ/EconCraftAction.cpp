/*
 * Aetherion economy: craft action (Economy BRD E7.1).
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#include "EconCraftAction.h"
#include "CraftPlanner.h"
#include "NeedsLedger.h"

#include "Config.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
bool CraftEnabled()
{
    static bool const enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Craft.Enabled", false);
    return enabled;
}

uint32 CraftScan()
{
    // The old cap of twelve truncated the recipe list before anything could
    // rank it, and it truncated in spell-map order - so a tailor who knows
    // forty recipes was choosing among an arbitrary twelve of them. That is
    // most of why 24 hours of crafting produced 1046 consumables and fifteen
    // wearable pieces.
    static uint32 const scan =
        uint32(std::max(sConfigMgr->GetOption<int32>("AiPlayerbot.Econ.Craft.Scan", 40), 1));
    return scan;
}

// What a finished craft is worth to the REALM, not to the crafter. Gear that
// somebody can wear and that the auction house can carry is the only
// profession output that turns into party item level; everything else is
// another stack of bandages, however useful. Non-wearable products all score
// zero, so among them the enumeration order the craft loop has been running on
// for ten thousand casts is preserved exactly.
uint32 CraftValue(CraftOption const& opt)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(opt.productItem);
    if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP)
        return 0;
    if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
        return 0;
    // Bind-on-pickup output never reaches the hub. The crafter can still wear
    // it, but this ranking exists to stock a market, so it is worth no more
    // here than a consumable.
    if (proto->Bonding == BIND_WHEN_PICKED_UP)
        return 0;
    return 1000 + proto->ItemLevel;
}

// Two modes: the default picks castable-anywhere recipes for idle beats; the
// "focus" mode (dispatched on arrival at an anvil, forge or fire) picks only
// focus-required ones - the bot is standing at the object, and the core's
// CheckCast confirms the match.
bool PickCastable(Player* bot, CraftOption& out, bool focusMode)
{
    std::vector<CraftOption> options;
    CraftPlanner::Enumerate(bot, options, CraftScan());
    uint32 bestValue = 0;
    bool found = false;
    for (CraftOption const& opt : options)
    {
        if (!opt.craftableNow)
            continue;
        if (focusMode != (opt.spellFocus != 0))
            continue;
        // Strictly greater, so when nothing outranks it the answer is still the
        // first castable recipe - the behaviour this action already had.
        uint32 const value = CraftValue(opt);
        if (found && value <= bestValue)
            continue;
        out = opt;
        bestValue = value;
        found = true;
    }
    return found;
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
