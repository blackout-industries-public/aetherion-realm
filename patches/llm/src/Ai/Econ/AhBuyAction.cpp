/*
 * Aetherion economy: auction-house buy action (Economy BRD E8.2).
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#include "AhBuyAction.h"
#include "NeedsLedger.h"

#include "BudgetValues.h"
#include "Config.h"
#include "ItemUsageValue.h"
#include "Opcodes.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <string>

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
    for (NeedsLedger::AhListing const& l : NeedsLedger::ListingsForHouse(HouseForTeam(bot->GetTeamId())))
    {
        if (!l.buyout || l.buyout > budget)
            continue;
        // The core's same-account check assumes offline owners and never fires
        // for permanently-online bots, so the guard lives here (BRD E8.2/D9).
        if (l.ownerAccount == myAccount)
            continue;

        std::string qualifier = std::to_string(l.item) + "," + std::to_string(l.randomPropertyId);
        ItemUsage usage = context->GetValue<ItemUsage>("item usage", qualifier)->Get();
        if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
            continue;

        WorldPacket* packet = new WorldPacket(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
        *packet << auctioneerGuid;
        *packet << l.auctionId;
        *packet << l.buyout;
        bot->GetSession()->QueuePacket(packet);

        NeedsLedger::LogEvent("ah_bid", bot->GetGUID().GetCounter(), l.item, l.count,
                              std::to_string(l.buyout));
        return true;
    }

    return false;
}
