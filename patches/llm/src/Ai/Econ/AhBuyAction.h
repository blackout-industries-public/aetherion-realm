/*
 * Aetherion economy: auction-house buy action (Economy BRD E8.2).
 *
 * Buyout-only on purpose: a proportional bid locks gold in mail-refund escrow,
 * while a buyout is one transaction that completes immediately.
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#ifndef AETHERION_AHBUYACTION_H
#define AETHERION_AHBUYACTION_H

#include "Action.h"

class PlayerbotAI;

class AhBuyAction : public Action
{
public:
    AhBuyAction(PlayerbotAI* botAI) : Action(botAI, "ah buy") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
