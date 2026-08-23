/*
 * Aetherion economy: bank deposit action (Economy BRD E6.3a).
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#include "BankDepositAction.h"
#include "CraftPlanner.h"
#include "NeedsLedger.h"

#include "Bag.h"
#include "Config.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Opcodes.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
bool BankEnabled()
{
    // Config is immutable at runtime for this gate; a one-time read avoids a
    // map-lookup per action tick across the whole bot fleet.
    static bool const enabled =
        sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Bank.Enabled", false);
    return enabled;
}

uint32 BankMaxPerVisit()
{
    static uint32 const maxPerVisit =
        uint32(std::max(sConfigMgr->GetOption<int32>("AiPlayerbot.Econ.Bank.MaxPerVisit", 6), 0));
    return maxPerVisit;
}

// Every reagent id any known recipe consumes, from the same planner data the
// craft chain acts on. Depositing these would force a bank round-trip before
// the next craft, so they are the one category a crafter must keep on hand
// (BRD E6.3a plus the operator's no-pointless-hoarding rule).
void CollectOwnReagents(Player* bot, std::unordered_set<uint32>& out)
{
    std::vector<CraftOption> options;
    CraftPlanner::Enumerate(bot, options, 0);
    for (CraftOption const& option : options)
    {
        for (std::pair<uint32, uint32> const& reagent : option.reagents)
            out.insert(reagent.first);
        // Tools too, or the enchanter banks its own rod.
        for (uint32 tool : option.tools)
            out.insert(tool);
    }
}

bool Depositable(Item* item, std::unordered_set<uint32> const& ownReagents)
{
    ItemTemplate const* proto = item->GetTemplate();
    // Only trade goods qualify as strategic materials; gear, consumables and
    // quest items have their own disposal paths (equip/use/AH/vendor).
    if (proto->Class != ITEM_CLASS_TRADE_GOODS)
        return false;
    return ownReagents.find(proto->ItemId) == ownReagents.end();
}
}  // namespace

void BankDepositAction::CollectDepositItems(std::vector<Item*>& out, uint32 limit)
{
    std::unordered_set<uint32> ownReagents;
    CollectOwnReagents(bot, ownReagents);

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (out.size() >= limit)
            return;

        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (item && Depositable(item, ownReagents))
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
            if (item && Depositable(item, ownReagents))
                out.push_back(item);
        }
    }
}

bool BankDepositAction::isUseful()
{
    if (!BankEnabled() || !BankMaxPerVisit())
        return false;

    std::vector<Item*> probe;
    CollectDepositItems(probe, 1);
    return !probe.empty();
}

bool BankDepositAction::Execute(Event /*event*/)
{
    if (!BankEnabled())
        return false;

    // Same proximity rule the handlers re-check: both HandleBankerActivateOpcode
    // and the CanUseBank gate inside HandleAutoBankItemOpcode resolve the banker
    // through GetNPCIfCanInteractWith, so without one in range every queued
    // packet would be silently dropped - bail before enumerating recipes.
    GuidVector npcs = context->GetValue<GuidVector>("nearest npcs")->Get();
    ObjectGuid bankerGuid;
    for (ObjectGuid const guid : npcs)
    {
        if (bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_BANKER))
        {
            bankerGuid = guid;
            break;
        }
    }

    if (!bankerGuid)
        return false;

    std::vector<Item*> items;
    CollectDepositItems(items, BankMaxPerVisit());
    if (items.empty())
        return false;

    // BANKER_ACTIVATE must land first: HandleAutoBankItemOpcode's CanUseBank()
    // reads m_currentBankerGUID, which only SendShowBank (called from the
    // activate handler) sets. HandleBotPackets drains the queue FIFO on the
    // world thread, so activation is processed before every deposit below.
    WorldPacket* activate = new WorldPacket(CMSG_BANKER_ACTIVATE, 8);
    *activate << bankerGuid;
    bot->GetSession()->QueuePacket(activate);

    uint32 queued = 0;
    for (Item* item : items)
    {
        // Layout verified against WorldPackets::Bank::AutoBankItem::Read - two
        // uint8s, bag then slot, resolved by GetItemByPos(Bag, Slot): the bag
        // byte is INVENTORY_SLOT_BAG_0 for the backpack or the equipped bag's
        // slot otherwise, which is exactly what Item::GetBagSlot returns.
        WorldPacket* packet = new WorldPacket(CMSG_AUTOBANK_ITEM, 2);
        *packet << uint8(item->GetBagSlot());
        *packet << uint8(item->GetSlot());
        // Ownership passes to the session queue. A full bank is harmless: the
        // handler's CanBankItem fails into SendEquipError and the item stays in
        // the bags, so this event is queue-time optimistic, not a receipt.
        bot->GetSession()->QueuePacket(packet);

        NeedsLedger::LogEvent("bank_deposit", bot->GetGUID().GetCounter(),
                              item->GetTemplate()->ItemId, item->GetCount(), "");
        ++queued;
    }

    return queued > 0;
}
