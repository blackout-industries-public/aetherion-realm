#!/usr/bin/env python3
"""Drain finished quests before Idle re-rolls a new pastime.

Hand-in only runs inside the active RPG statuses, so a bot that completed its
objectives and then idled kept the reward forever. Measured before this patch: 247
quest accepts per census interval against 8 rewards, and a standing backlog of ~2600
COMPLETE quests across the realm.
"""
import sys

path = sys.argv[1]
src = open(path).read()

ANCHOR = """        case RPG_IDLE:
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});"""

REPLACEMENT = """        case RPG_IDLE:
        {
            // Finished quests are handed in before a new pastime is rolled. DoQuest
            // already routes COMPLETE quests to their reward POI; the gap was that an
            // idle bot never re-entered DoQuest while holding one. lowPriorityQuest
            // skips turn-ins DoCompletedQuest has already abandoned this session, so
            // this cannot ping-pong with that timeout.
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = bot->GetQuestSlotQuestId(slot);
                if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                    continue;
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (!quest)
                    continue;
                info.ChangeToDoQuest(questId, quest);
                return true;
            }
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});
        }"""

if REPLACEMENT in src:
    print("quest turn-in drain already applied")
    sys.exit(0)
assert src.count(ANCHOR) == 1, "RPG_IDLE anchor missing or ambiguous"
open(path, "w").write(src.replace(ANCHOR, REPLACEMENT, 1))
print("patched NewRpgAction.cpp (idle quest turn-in drain)")
