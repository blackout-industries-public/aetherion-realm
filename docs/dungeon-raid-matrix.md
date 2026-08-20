# Dungeon and Raid Test Matrix (BRD s12)

The BRD is explicit that Playerbots' claim of "most raids supported" is to be
verified empirically, not assumed. Record every attempt, including the failures -
the failure modes are what tune the config.

Target: a human can form a bot group and complete representative Classic,
Outland, Northrend and heroic dungeons. Raids are validated per encounter.

| Instance | Boss | Bot roles (T/H/D) | Result | Failure mode | Intervention required |
|---|---|---|---|---|---|
| Deadmines | Van Cleef | | | | |
| Wailing Caverns | Mutanus | | | | |
| Scarlet Monastery | Whitemane | | | | |
| Hellfire Ramparts | Omor | | | | |
| The Underbog | The Black Stalker | | | | |
| Utgarde Keep | Ingvar | | | | |
| The Nexus | Keristrasza | | | | |
| Utgarde Keep (heroic) | Ingvar | | | | |
| Violet Hold (heroic) | Cyanigosa | | | | |
| Naxxramas 10 | Patchwerk | | | | |
| Ulduar 10 | Flame Leviathan | | | | |

## Notes

- Record the bot composition actually used, not the one intended - Playerbots
  picks from the available random population.
- "Intervention required" means anything a human had to do that a real group
  would not have needed (summoning, manual pulls, resetting a stuck bot).

## How to actually form a bot raid

Learned the hard way; the distinction is not documented upstream.

**Random bots cannot be used as a raid team.** You can invite them with `/invite`
(set `AiPlayerbot.GroupInvitationPermission = 2` so they always accept), but every
management command beyond `add`/`remove` is refused with
"ERROR: You can only use this command on addclass bots." They also keep running
their own RPG behaviour - `RpgStatusProbWeight.DoQuest` is 60 - so they wander off
across the map instead of following.

**Use the addclass pool instead.** Playerbots reserves a separate set of accounts
(`acore_playerbots.playerbots_account_type`, `account_type = 2`) whose characters
stay offline and exist purely to be adopted:

```
.playerbots bot addclass warrior      # warrior/paladin/hunter/rogue/priest/
                                      # shaman/mage/warlock/druid/dk
.playerbots bot init=epic *           # levels to YOUR level AND gears in epics
.playerbots bot refresh=raid *
```

`init=<quality>` builds its factory with `master->GetLevel()`, so a level 3 bot from
the pool becomes a geared level 80. `levelup` uses `bot->GetLevel()` instead and so
will NOT level anything - it only re-rolls at the bot's current level.

Constraints worth knowing:
- `AiPlayerbot.MaxAddedBots` (40) caps the team; enough for 25-man.
- Commands are refused while either you or the bot is in combat.
- `addclass` applies no level filter when picking from the pool, which does not
  matter once `init=` runs, but does mean the bot arrives underlevelled.
