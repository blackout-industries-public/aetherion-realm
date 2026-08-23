/*
 * Aetherion economy: craft action (Economy BRD E7.1).
 *
 * Casts a known profession recipe whose reagents are in the bags. V1 takes
 * only recipes without a spell-focus requirement (no anvil/forge yet) - the
 * core's CheckCast still validates tools, bag space and reagents, so a bad
 * pick fails cleanly rather than cheating.
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#ifndef AETHERION_ECONCRAFTACTION_H
#define AETHERION_ECONCRAFTACTION_H

#include "Action.h"

class PlayerbotAI;

class EconCraftAction : public Action
{
public:
    EconCraftAction(PlayerbotAI* botAI) : Action(botAI, "econ craft") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
