/*
 * Aetherion economy: auction-house sell action (Economy BRD E4.1a).
 *
 * Posts bag items classified ITEM_USAGE_AH to a nearby auctioneer by building
 * the real CMSG_AUCTION_SELL_ITEM packet and queueing it on the bot session.
 * Queueing instead of calling the auction manager directly keeps execution on
 * the world thread via HandleBotPackets, the same path a real client takes,
 * so every core-side validity check still runs. Off by default behind
 * AiPlayerbot.Econ.Ah.Enabled.
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#ifndef AETHERION_AHSELLACTION_H
#define AETHERION_AHSELLACTION_H

#include "Action.h"

#include <vector>

class Item;
class PlayerbotAI;

class AhSellAction : public Action
{
public:
    AhSellAction(PlayerbotAI* botAI) : Action(botAI, "ah sell") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // Bounded collector so the isUseful probe (limit 1) stays cheap while
    // Execute reuses the same filter with the per-visit cap.
    void CollectAhItems(std::vector<Item*>& out, uint32 limit);
};

#endif
