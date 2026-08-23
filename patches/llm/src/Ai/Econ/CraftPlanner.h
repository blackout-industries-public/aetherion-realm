/*
 * Aetherion economy: recipe intelligence for the craft chain (Economy BRD E7).
 *
 * Read-only planner: given a bot, enumerate the profession recipes it knows -
 * which spell, which item it produces, which reagents it consumes, and which
 * of those reagents the bags are short. It never casts, never touches the DB,
 * and never mutates the bot, so any code path that already owns the Player can
 * call it; acting on an option (casting the recipe, shopping for materials)
 * stays with the caller. That split lets the needs ledger raise material needs
 * from the same data a future craft action consumes.
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#ifndef AETHERION_CRAFTPLANNER_H
#define AETHERION_CRAFTPLANNER_H

#include "Define.h"

#include <utility>
#include <vector>

class Player;

// One craftable recipe as seen from a specific bot's bags at call time.
struct CraftOption
{
    uint32 spellId = 0;
    uint32 productItem = 0;
    uint32 productCount = 1;
    // (itemId, countRequired) straight from spell data.
    std::vector<std::pair<uint32, uint32>> reagents;
    // (itemId, countShort) after subtracting bag stock - empty means the bot
    // holds everything the recipe needs right now.
    std::vector<std::pair<uint32, uint32>> missing;
    // Required-but-not-consumed tool items from the spell Totem slots.
    std::vector<uint32> tools;
    bool craftableNow = false;
};

class CraftPlanner
{
public:
    // Appends up to `limit` options (0 means uncapped) for the bot's active,
    // spec-visible profession spells that create a concrete item. Bag counts
    // exclude the bank: a craft decided in the field can only draw on what the
    // bot carries.
    static void Enumerate(Player* bot, std::vector<CraftOption>& out, uint32 limit);
};

#endif
