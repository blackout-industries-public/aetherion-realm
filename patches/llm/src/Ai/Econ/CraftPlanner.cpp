/*
 * Aetherion economy: recipe intelligence for the craft chain (Economy BRD E7).
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#include "CraftPlanner.h"

#include "DBCStores.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
    // A spell only counts as a profession recipe when one of its skill lines
    // is a real profession (primary, or fishing/cooking/first aid). This keeps
    // class conjures, quest gimmicks, and engineering trinket procs that also
    // carry create-item effects out of the plan.
    bool IsProfessionRecipe(uint32 spellId)
    {
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (SkillLineAbilityMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
            if (IsProfessionSkill(itr->second->SkillLine))
                return true;

        return false;
    }
}

void CraftPlanner::Enumerate(Player* bot, std::vector<CraftOption>& out, uint32 limit)
{
    if (!bot)
        return;

    for (PlayerSpellMap::const_iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        if (limit && out.size() >= limit)
            return;

        // Removed entries are gone, and Active=false is also how the core
        // marks lower ranks superseded by a learned upgrade - skipping them
        // dedupes rank chains for free.
        if (itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active)
            continue;

        if (!(itr->second->specMask & bot->GetActiveSpecMask()))
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itr->first);
        if (!spellInfo || spellInfo->IsPassive())
            continue;

        if (!IsProfessionRecipe(itr->first))
            continue;

        // Resolve the created item. CREATE_ITEM_2 with an empty ItemType is
        // loot-table crafting whose product is rolled at cast time, which a
        // planner cannot value - only concrete products pass.
        uint32 productItem = 0;
        uint32 productCount = 1;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint32 effect = spellInfo->Effects[i].Effect;
            if ((effect == SPELL_EFFECT_CREATE_ITEM || effect == SPELL_EFFECT_CREATE_ITEM_2) &&
                spellInfo->Effects[i].ItemType)
            {
                productItem = spellInfo->Effects[i].ItemType;

                // The cast path creates CalcValue items per cast, never fewer
                // than one (Spell::DoCreateItem addNumber).
                int32 calc = spellInfo->Effects[i].CalcValue(bot);
                productCount = calc > 1 ? uint32(calc) : 1;
                break;
            }
        }

        if (!productItem)
            continue;

        CraftOption option;
        option.spellId = itr->first;
        option.productItem = productItem;
        option.productCount = productCount;

        for (uint8 x = 0; x < MAX_SPELL_REAGENTS; ++x)
        {
            // Reagent is signed in spell data; non-positive slots are padding.
            if (spellInfo->Reagent[x] <= 0 || !spellInfo->ReagentCount[x])
                continue;

            uint32 itemId = uint32(spellInfo->Reagent[x]);
            uint32 required = spellInfo->ReagentCount[x];
            option.reagents.emplace_back(itemId, required);

            // Bags only (no bank): a craft decided in the field can only draw
            // on carried stock.
            uint32 have = bot->GetItemCount(itemId, false);
            if (have < required)
                option.missing.emplace_back(itemId, required - have);
        }

        // A profession create-item spell without reagents is bad spell data;
        // the module's own craft picker refuses those, so mirror it.
        if (option.reagents.empty())
            continue;

        option.craftableNow = option.missing.empty();
        out.push_back(std::move(option));
    }
}
