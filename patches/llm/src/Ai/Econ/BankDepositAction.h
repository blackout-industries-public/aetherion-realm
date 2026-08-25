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
class Player;
class PlayerbotAI;

class BankDepositAction : public Action
{
public:
    BankDepositAction(PlayerbotAI* botAI) : Action(botAI, "bank deposit") {}

    bool Execute(Event event) override;
    bool isUseful() override;

    // How many bagged items this action would actually bank, capped at the caller's
    // limit so a mere "is there anything?" probe stays cheap. Public because the
    // errand that decides to walk a bot to a banker and the action that runs when it
    // arrives have to agree: a trip decided on a different test than the one waiting
    // at the other end is a bot that walks across a city and does nothing.
    static uint32 CountDepositable(Player* bot, uint32 limit);

    // True when the bot's own vault holds a reagent or tool one of its recipes is
    // short of. No cache is needed for this: a logged-in character carries its bank
    // contents in the same item slots the withdrawal pass already reads, so the
    // question can be asked anywhere - which is the whole point, since the answer is
    // what decides to walk to a banker in the first place.
    static bool HasVaultedReagent(Player* bot);

private:
    // Bounded collector so the isUseful probe (limit 1) stays cheap while
    // Execute reuses the same filter with the per-visit cap.
    void CollectDepositItems(std::vector<Item*>& out, uint32 limit);
};

#endif
