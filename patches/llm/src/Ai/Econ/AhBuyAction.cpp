/*
 * Aetherion economy: auction-house buy action (Economy BRD E8.2).
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#include "AhBuyAction.h"
#include "CraftPlanner.h"
#include "NeedsLedger.h"

#include "BudgetValues.h"
#include "Config.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
bool AhBuyEnabled()
{
    static bool const enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Ah.Buy.Enabled", false);
    return enabled;
}

// AuctionHouse.dbc house ids; the mirror tags every listing with its house and
// the handler resolves the auction inside the house of the auctioneer the bot
// stands at, so filtering by faction here prevents doomed cross-house bids.
uint8 HouseForTeam(TeamId team)
{
    return team == TEAM_ALLIANCE ? 2 : 6;
}

// E11: how many recipes a shopper weighs before buying reagents. Eight was the
// same truncation the craft picker suffered from - it cut the list in spell-map
// order, so a smith with thirty recipes shopped for whichever eight came first.
uint32 MatsScan()
{
    static uint32 const scan =
        uint32(std::max(sConfigMgr->GetOption<int32>("AiPlayerbot.Econ.Craft.Scan", 40), 1));
    return scan;
}

// Whether a recipe's output is gear the market can carry. Measured: this realm
// has essentially no loose tradeable gear - 17 such items across 2500 bots, ten
// of them already soulbound - because a bot either wears what it loots or the
// drop was bind-on-pickup. Production is therefore the ONLY way gear reaches the
// auction house, which makes a crafter's shopping list the front of that queue.
bool WearableProduct(uint32 itemId)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP)
        return false;
    if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
        return false;
    return proto->Bonding != BIND_WHEN_PICKED_UP;
}
}  // namespace

bool AhBuyAction::isUseful()
{
    return AhBuyEnabled();
}

bool AhBuyAction::Execute(Event /*event*/)
{
    if (!AhBuyEnabled())
        return false;

    GuidVector npcs = context->GetValue<GuidVector>("nearest npcs")->Get();
    ObjectGuid auctioneerGuid;
    for (ObjectGuid const guid : npcs)
    {
        if (bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER))
        {
            auctioneerGuid = guid;
            break;
        }
    }
    if (!auctioneerGuid)
        return false;

    uint32 const myAccount = bot->GetSession()->GetAccountId();
    uint32 const budget =
        context->GetValue<uint32>("free money for", uint32(NeedMoneyFor::gear))->Get();
    if (!budget)
        return false;

    // One buyout per visit: the mirror is up to a minute stale, so a shopping
    // spree against it would race other buyers; a failed bid is harmless (the
    // handler just rejects) but wasteful in packets.
    //
    // E11: best rather than first. Mirror order is auction order, so the first
    // cut spent the one buyout on whichever upgrade happened to be listed
    // earliest - invisible while the market held nine wearable items, and a
    // real difference now that the shredder feeds it instead of the bin. Item
    // level ranks them because the fit question is already settled by the time a
    // listing gets here: EQUIP or REPLACE means class, slot and stat weights
    // have all agreed.
    std::vector<NeedsLedger::AhListing> const listings =
        NeedsLedger::ListingsForHouse(HouseForTeam(bot->GetTeamId()));
    NeedsLedger::AhListing const* best = nullptr;
    uint32 bestIlvl = 0;
    for (NeedsLedger::AhListing const& l : listings)
    {
        if (!l.buyout || l.buyout > budget)
            continue;
        // The core's same-account check assumes offline owners and never fires
        // for permanently-online bots, so the guard lives here (BRD E8.2/D9).
        if (l.ownerAccount == myAccount)
            continue;

        // Cheap rejections before the expensive one: answering "item usage"
        // costs a constructed Item, and most of a live market is herbs and
        // arrows. Ranking first also means the question is asked only of
        // listings that could still win.
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(l.item);
        if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP)
            continue;
        if (uint32(proto->ItemLevel) <= bestIlvl)
            continue;

        std::string qualifier = std::to_string(l.item) + "," + std::to_string(l.randomPropertyId);
        ItemUsage usage = context->GetValue<ItemUsage>("item usage", qualifier)->Get();
        if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
            continue;

        best = &l;
        bestIlvl = proto->ItemLevel;
    }

    if (best)
    {
        WorldPacket* packet = new WorldPacket(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
        *packet << auctioneerGuid;
        *packet << best->auctionId;
        *packet << best->buyout;
        bot->GetSession()->QueuePacket(packet);

        NeedsLedger::LogEvent("ah_bid", bot->GetGUID().GetCounter(), best->item, best->count,
                              std::to_string(best->buyout));
        return true;
    }

    // E7.4: no gear worth buying - shop for the reagents the bot's own recipes
    // are short of instead. This is the demand half of the gatherer-to-crafter
    // loop: a gatherer's herb listing meets an alchemist's shortfall here.
    {
        std::vector<CraftOption> options;
        CraftPlanner::Enumerate(bot, options, MatsScan());
        uint32 const matsBudget =
            context->GetValue<uint32>("free money for", uint32(NeedMoneyFor::tradeskill))->Get();
        // Two rounds over the same list: recipes that make gear first, then
        // everything else exactly as before. A crafter that spends its one
        // purchase on bandage cloth while a mail-armour recipe sits two bars
        // short is the gatherer-to-crafter loop feeding the wrong end - and gear
        // is the only end of it that ever becomes party item level.
        if (matsBudget)
            for (bool wearableRound : {true, false})
                for (CraftOption const& opt : options)
                {
                    if (WearableProduct(opt.productItem) != wearableRound)
                        continue;
                    for (auto const& [itemId, shortBy] : opt.missing)
                        for (NeedsLedger::AhListing const& l : listings)
                        {
                            if (l.item != itemId || !l.buyout || l.buyout > matsBudget)
                                continue;
                            if (l.ownerAccount == myAccount)
                                continue;
                            WorldPacket* packet =
                                new WorldPacket(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
                            *packet << auctioneerGuid;
                            *packet << l.auctionId;
                            *packet << l.buyout;
                            bot->GetSession()->QueuePacket(packet);
                            NeedsLedger::LogEvent("ah_bid_mats", bot->GetGUID().GetCounter(),
                                                  l.item, l.count, std::to_string(l.buyout));
                            return true;
                        }
                }
    }

    return false;
}
