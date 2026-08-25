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
#include "Guild.h"
#include "GuildMgr.h"
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

uint32 BankDepositAction::CountDepositable(Player* bot, uint32 limit)
{
    if (!bot || !limit)
        return 0;

    // Cheap gate first. CollectOwnReagents enumerates every recipe the bot knows,
    // and this runs for most of the realm on every needs pass; a bot carrying no
    // trade goods at all can be answered by a bag walk alone and never pay for it.
    bool anyTradeGoods = false;
    auto isTradeGood = [](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        return proto && proto->Class == ITEM_CLASS_TRADE_GOODS;
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; !anyTradeGoods && slot < INVENTORY_SLOT_ITEM_END;
         ++slot)
        anyTradeGoods = isTradeGood(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START;
         !anyTradeGoods && bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
        if (!bag)
            continue;
        for (uint32 slot = 0; !anyTradeGoods && slot < bag->GetBagSize(); ++slot)
            anyTradeGoods = isTradeGood(bag->GetItemByPos(slot));
    }
    if (!anyTradeGoods)
        return 0;

    std::unordered_set<uint32> ownReagents;
    CollectOwnReagents(bot, ownReagents);

    uint32 found = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (item && Depositable(item, ownReagents) && ++found >= limit)
            return found;
    }

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
        if (!bag)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
        {
            Item* item = bag->GetItemByPos(slot);
            if (item && Depositable(item, ownReagents) && ++found >= limit)
                return found;
        }
    }
    return found;
}

bool BankDepositAction::HasVaultedReagent(Player* bot)
{
    if (!bot)
        return false;

    // Cheap gate first, for the same reason the deposit count has one: an empty
    // vault cannot supply anything, and asking the planner is the expensive half.
    auto scanVault = [bot](auto&& predicate)
    {
        for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (predicate(item))
                    return true;
        for (uint8 bagSlot = BANK_SLOT_BAG_START; bagSlot < BANK_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(slot))
                        if (predicate(item))
                            return true;
        return false;
    };

    if (!scanVault([](Item*) { return true; }))
        return false;

    std::unordered_set<uint32> wanted;
    std::vector<CraftOption> options;
    CraftPlanner::Enumerate(bot, options, 0);
    for (CraftOption const& opt : options)
    {
        for (std::pair<uint32, uint32> const& missing : opt.missing)
            wanted.insert(missing.first);
        for (uint32 tool : opt.tools)
            if (!bot->GetItemCount(tool, false))
                wanted.insert(tool);
    }
    if (wanted.empty())
        return false;

    return scanVault([&wanted](Item* item) { return wanted.count(item->GetEntry()) != 0; });
}

void BankDepositAction::CollectDepositItems(std::vector<Item*>& out, uint32 limit)
{
    std::unordered_set<uint32> ownReagents;
    CollectOwnReagents(bot, ownReagents);

    // E9: for a guilded bot the disposal split has already decided which items are
    // the vault's, weighing the bot's own solvency first and then what the vault is
    // actually short of. Honouring it here is what stops a bot walking to a banker
    // and depositing the fifteenth stack of copper ore the guild did not need. An
    // unguilded bot gets an empty plan and the original trade-goods rule below,
    // which is exactly the behaviour it has today.
    std::unordered_set<ObjectGuid> planned;
    NeedsLedger::PlanDisposal(bot, planned);
    bool const guilded = bot->GetGuildId() != 0 && !planned.empty();

    auto const wanted = [&](Item* item)
    {
        if (!item)
            return false;
        return guilded ? planned.count(item->GetGUID()) != 0
                       : Depositable(item, ownReagents);
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (out.size() >= limit)
            return;

        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (wanted(item))
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
            if (wanted(item))
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

    // E6.3b: reagents a recipe is short of come OUT of the vault first - this
    // is also how a mis-banked tool (the Corrioa incident) finds its way home.
    std::vector<Item*> withdrawals;
    {
        std::unordered_set<uint32> wanted;
        std::vector<CraftOption> options;
        CraftPlanner::Enumerate(bot, options, 0);
        for (CraftOption const& opt : options)
        {
            for (auto const& missing : opt.missing)
                wanted.insert(missing.first);
            for (uint32 tool : opt.tools)
                if (!bot->GetItemCount(tool, false))
                    wanted.insert(tool);
        }
        if (!wanted.empty())
        {
            for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (wanted.count(item->GetEntry()))
                        withdrawals.push_back(item);
            for (uint8 bagSlot = BANK_SLOT_BAG_START; bagSlot < BANK_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        if (Item* item = bag->GetItemByPos(slot))
                            if (wanted.count(item->GetEntry()))
                                withdrawals.push_back(item);
        }
    }

    // E8.2: reagents still missing after the personal vault pass come out of
    // the GUILD vault - the other half of "members stock it, crafters use it".
    // The slot comes from the ledger's five-minute cache of guild_bank_item;
    // a stale slot makes the core's own checks no-op or move a different
    // stack, both of which the economy absorbs. The daily-withdrawal
    // allowance stays entirely core-enforced.
    std::vector<std::pair<uint8, uint8>> vaultPulls;
    if (Guild* g = bot->GetGuildId() ? sGuildMgr->GetGuildById(bot->GetGuildId()) : nullptr)
    {
        std::unordered_set<uint32> stillMissing;
        std::vector<CraftOption> options;
        CraftPlanner::Enumerate(bot, options, 0);
        for (CraftOption const& opt : options)
            for (auto const& missing : opt.missing)
                stillMissing.insert(missing.first);
        for (Item* item : withdrawals)
            stillMissing.erase(item->GetEntry());

        for (uint32 const entry : stillMissing)
        {
            if (vaultPulls.size() >= 3)
                break;
            uint8 tab = 0, slot = 0;
            if (!NeedsLedger::FindVaultReagent(bot->GetGuildId(), entry, tab, slot))
                continue;
            NeedsLedger::LogEvent("guild_bank_withdraw", bot->GetGUID().GetCounter(),
                                  entry, 1, std::to_string(tab));
            vaultPulls.emplace_back(tab, slot);
        }
        // NULL_BAG(0)/NULL_SLOT(255) = auto-store: the player side finds its
        // own bag space, same as the client's own withdraw path.
        for (auto const& [tab, slot] : vaultPulls)
            g->SwapItemsWithInventory(bot, true, tab, slot, 0, 255, 0);
    }

    std::vector<Item*> items;
    CollectDepositItems(items, BankMaxPerVisit());

    // E8: the guild vault, same banker visit. Surplus trade goods go to the
    // shared Materials tab instead of the personal vault, and a slice of
    // loose gold is tithed; repairs draw it back out (RepairAllAction). The
    // direct Guild calls follow the module's own GuildBankAction - rights,
    // tab existence, and funds stay entirely rank-enforced by Guild itself.
    Guild* guild = bot->GetGuildId() ? sGuildMgr->GetGuildById(bot->GetGuildId()) : nullptr;
    // Vault layout mirrors the seeded tabs: 0 Materials, 1 Consumables,
    // 2 Gear. Anything else stays personal.
    auto const tabFor = [](ItemTemplate const* proto) -> int
    {
        if (proto->Class == ITEM_CLASS_TRADE_GOODS)
            return 0;
        if (proto->Class == ITEM_CLASS_CONSUMABLE)
            return 1;
        if ((proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) &&
            proto->Quality >= ITEM_QUALITY_UNCOMMON)
            return 2;
        return -1;
    };
    std::vector<std::pair<Item*, uint8>> guildMats;
    if (guild)
        for (auto it = items.begin(); it != items.end() && guildMats.size() < 6;)
        {
            int const tab = tabFor((*it)->GetTemplate());
            if (tab >= 0 &&
                guild->MemberHasTabRights(bot->GetGUID(), uint8(tab),
                                          GUILD_BANK_RIGHT_DEPOSIT_ITEM))
            {
                guildMats.emplace_back(*it, uint8(tab));
                it = items.erase(it);
            }
            else
                ++it;
        }

    uint32 tithe = 0;
    if (guild)
    {
        static uint32 const floorCopper = 10000u * uint32(std::max(
            sConfigMgr->GetOption<int32>("AiPlayerbot.Econ.Guild.TitheFloorGold", 40), 0));
        static int32 const tithePct =
            sConfigMgr->GetOption<int32>("AiPlayerbot.Econ.Guild.TithePct", 10);
        if (tithePct > 0 && bot->GetMoney() > floorCopper)
        {
            tithe = uint32(uint64(bot->GetMoney() - floorCopper) * uint32(tithePct) / 100u);
            // At least a gold or nothing, at most 20 per visit: pocket change
            // spams the ledger and a whale should not empty into the vault.
            tithe = std::min(tithe, 200000u);
            if (tithe < 10000u)
                tithe = 0;
        }
    }

    if (items.empty() && withdrawals.empty() && guildMats.empty() && vaultPulls.empty() &&
        !tithe)
        return false;

    if (tithe)
    {
        guild->HandleMemberDepositMoney(bot->GetSession(), tithe);
        NeedsLedger::LogEvent("guild_tithe", bot->GetGUID().GetCounter(), 0, tithe, "");
    }

    for (auto const& [item, tab] : guildMats)
    {
        // Logged before the swap: the Item* is the bot's bag-side object and
        // does not survive the move. The detail carries why this item went to the
        // vault rather than the auction house - what the guild already held against
        // what counts as plenty - so the dashboard can show the reasoning and not
        // only the outcome.
        NeedsLedger::LogEvent(
            "guild_bank_deposit", bot->GetGUID().GetCounter(), item->GetEntry(),
            item->GetCount(),
            "tab " + std::to_string(tab) + " short: held " +
                std::to_string(NeedsLedger::VaultStock(bot->GetGuildId(), item->GetEntry())) +
                " of " +
                std::to_string(std::max<uint32>(1, item->GetTemplate()->GetMaxStackSize()) *
                               NeedsLedger::VaultPlentyStacks()));
        // 255 = NULL_SLOT: the vault picks the slot, exactly as the module's
        // guild bank action does. A guild without that tab is a silent no-op.
        guild->SwapItemsWithInventory(bot, false, tab, 255,
                                      item->GetBagSlot(), item->GetSlot(), 0);
    }

    if (items.empty() && withdrawals.empty())
        return !guildMats.empty() || !vaultPulls.empty() || tithe > 0;

    // BANKER_ACTIVATE must land first: HandleAutoBankItemOpcode's CanUseBank()
    // reads m_currentBankerGUID, which only SendShowBank (called from the
    // activate handler) sets. HandleBotPackets drains the queue FIFO on the
    // world thread, so activation is processed before every deposit below.
    WorldPacket* activate = new WorldPacket(CMSG_BANKER_ACTIVATE, 8);
    *activate << bankerGuid;
    bot->GetSession()->QueuePacket(activate);

    // Withdrawals ride the bidirectional AUTOSTORE opcode: a bank-side source
    // slot moves the item back to the bags.
    for (Item* item : withdrawals)
    {
        WorldPacket* packet = new WorldPacket(CMSG_AUTOSTORE_BANK_ITEM, 2);
        *packet << uint8(item->GetBagSlot());
        *packet << uint8(item->GetSlot());
        bot->GetSession()->QueuePacket(packet);
        NeedsLedger::LogEvent("bank_withdraw", bot->GetGUID().GetCounter(), item->GetEntry(),
                              item->GetCount(), "");
    }

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
