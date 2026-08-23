/*
 * Aetherion economy: bank deposit action (Economy BRD E6.3a).
 *
 * A bot standing at a banker deposits strategic trade goods into its personal
 * bank by queueing the real CMSG_BANKER_ACTIVATE + CMSG_AUTOBANK_ITEM packets
 * on its session. Queueing instead of calling Player::BankItem directly keeps
 * execution on the world thread via HandleBotPackets, the same path a real
 * client takes, so proximity, capacity and persistence checks all run
 * core-side. Reagents the bot's own recipes consume stay in the bags: a
 * crafter keeps its working stock, everything else banked is deliberate
 * hoard-avoidance, not accumulation. Off by default behind
 * AiPlayerbot.Econ.Bank.Enabled.
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#ifndef AETHERION_BANKDEPOSITACTION_H
#define AETHERION_BANKDEPOSITACTION_H

#include "Action.h"

#include <vector>

class Item;
class PlayerbotAI;

class BankDepositAction : public Action
{
public:
    BankDepositAction(PlayerbotAI* botAI) : Action(botAI, "bank deposit") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // Bounded collector so the isUseful probe (limit 1) stays cheap while
    // Execute reuses the same filter with the per-visit cap.
    void CollectDepositItems(std::vector<Item*>& out, uint32 limit);
};

#endif
