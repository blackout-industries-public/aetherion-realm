/*
 * Aetherion: the rare hunt's closing move (Economy BRD E10).
 *
 * A hunter bot walks to a named rare under VERDICT_RARE. Most of the time the
 * rare notices first and combat starts on approach, with no help from here.
 * This covers the case the approach cannot: a rare far enough below the bot
 * that it never aggroes, which would otherwise leave the bot standing on top
 * of its own target doing nothing.
 *
 * Derived from AttackAction so the engagement runs the module's real combat
 * entry - selection, current target, loot registration, engine switch - rather
 * than a private imitation of it. The action re-resolves the target from the
 * needs mirror by DB spawn id, so it can only ever start the fight the world
 * thread already sized against this bot's level; a rare the bot merely wandered
 * past is not its hunt. Off by default behind AiPlayerbot.Econ.Rare.Enabled.
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#ifndef AETHERION_RAREHUNTACTION_H
#define AETHERION_RAREHUNTACTION_H

#include "AttackAction.h"

class PlayerbotAI;

class RareHuntAction : public AttackAction
{
public:
    RareHuntAction(PlayerbotAI* botAI) : AttackAction(botAI, "rare hunt") {}

    bool Execute(Event event) override;
};

#endif
