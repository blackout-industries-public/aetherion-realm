/*
 * Aetherion economy: auction-house sell action (Economy BRD E4.1a).
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#include "AhSellAction.h"
#include "NeedsLedger.h"

#include "Bag.h"
#include "Config.h"
#include "Creature.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "Opcodes.h"
#include "Player.h"
#include "Playerbots.h"
#include "Random.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
// Duration is sent in MINUTES: HandleAuctionSellItem multiplies by MINUTE and
// only accepts 1/2/4 x MIN_AUCTION_TIME, i.e. 720, 1440 or 2880. 24h keeps
// bot listings cycling daily without relisting churn.
constexpr uint32 AH_DURATION_MINUTES = 1440;

bool AhEnabled()
{
    // Config is immutable at runtime for this gate; a one-time read avoids a
    // map-lookup per action tick across the whole bot fleet.
    static bool const enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Ah.Enabled", false);
    return enabled;
}

uint32 AhMaxPerVisit()
{
    static uint32 const maxPerVisit =
        uint32(std::max(sConfigMgr->GetOption<int32>("AiPlayerbot.Econ.Ah.MaxPerVisit", 4), 0));
    return maxPerVisit;
}

// Mirrors the reject conditions in WorldSession::HandleAuctionSellItem so a
// queued packet never bounces server-side: soulbound/untradeable, non-empty
// bags, conjured items and items with a duration all fail there, and quest
// items are excluded as a policy choice even where the core would accept them.
bool AhSellable(Item* item)
{
    ItemTemplate const* proto = item->GetTemplate();
    if (item->IsSoulBound() || !item->CanBeTraded() || item->IsNotEmptyBag())
        return false;
    if (proto->HasFlag(ITEM_FLAG_CONJURED) || item->GetUInt32Value(ITEM_FIELD_DURATION))
        return false;
    if (proto->Class == ITEM_CLASS_QUEST)
        return false;
    // The handler drops per-item counts above 1000 outright.
    if (!item->GetCount() || item->GetCount() > 1000)
        return false;
    return true;
}

// Vendor prices are the only universally available anchor: SellPrice*3 is a
// sane floor above vendoring, BuyPrice covers goods whose SellPrice is zero
// or trivial. The urand spread keeps the fleet from posting identical walls.
uint32 PriceBuyout(ItemTemplate const* proto, uint32 count)
{
    uint64 unit = std::max<uint64>(uint64(proto->SellPrice) * 3, proto->BuyPrice);
    uint64 buyout = unit * count * urand(95, 115) / 100;
    if (buyout > MAX_MONEY_AMOUNT)
        buyout = MAX_MONEY_AMOUNT;
    return uint32(buyout);
}
}  // namespace

void AhSellAction::CollectAhItems(std::vector<Item*>& out, uint32 limit)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (out.size() >= limit)
            return;

        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !AhSellable(item))
            continue;

        if (context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get() == ITEM_USAGE_AH)
            out.push_back(item);
    }

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
        if (!bag)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
        {
            if (out.size() >= limit)
                return;

            Item* item = bag->GetItemByPos(slot);
            if (!item || !AhSellable(item))
                continue;

            if (context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get() == ITEM_USAGE_AH)
                out.push_back(item);
        }
    }
}

bool AhSellAction::isUseful()
{
    if (!AhEnabled() || !AhMaxPerVisit())
        return false;

    std::vector<Item*> probe;
    CollectAhItems(probe, 1);
    return !probe.empty();
}

bool AhSellAction::Execute(Event /*event*/)
{
    if (!AhEnabled())
        return false;

    // Same proximity rule the handler re-checks: without an interactable
    // auctioneer the packet would be silently dropped, so bail before pricing.
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

    std::vector<Item*> items;
    CollectAhItems(items, AhMaxPerVisit());
    if (items.empty())
        return false;

    uint32 posted = 0;
    for (Item* item : items)
    {
        uint32 count = item->GetCount();
        uint32 buyout = PriceBuyout(item->GetTemplate(), count);
        // A zero bid is rejected by the handler; items with no vendor anchor
        // have no sane price either, so leave them alone.
        if (!buyout)
            continue;

        uint32 bid = std::max<uint32>(buyout * 4 / 5, 1);

        // One item per packet: the multi-item path in HandleAuctionSellItem
        // demands identical entries plus a merged stack, which full unsplit
        // stacks do not satisfy in general. Layout verified against the
        // handler: guid, count32, [guid64, count32] x count32, bid, buyout,
        // etime-in-minutes.
        WorldPacket* packet = new WorldPacket(CMSG_AUCTION_SELL_ITEM, 8 + 4 + 12 + 4 + 4 + 4);
        *packet << auctioneerGuid;
        *packet << uint32(1);
        *packet << item->GetGUID();
        *packet << count;
        *packet << bid;
        *packet << buyout;
        *packet << AH_DURATION_MINUTES;
        // Ownership passes to the session queue; HandleBotPackets drains it
        // on the world thread, which is what makes skipping sAuctionMgr safe.
        bot->GetSession()->QueuePacket(packet);

        NeedsLedger::LogEvent("ah_post", bot->GetGUID().GetCounter(), item->GetTemplate()->ItemId,
                              count, std::to_string(buyout));
        ++posted;
    }

    return posted > 0;
}
