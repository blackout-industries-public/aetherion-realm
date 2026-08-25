#!/usr/bin/env bash
# Grant the Frozen Halls attunement to the level-80 BOT population.
#
# Pit of Saron (map 658) and Halls of Reflection (map 668) are gated by
# dungeon_access_requirements on the quest chain that runs through them:
#   Echoes of Tortured Souls  - 24499 Alliance / 24511 Horde -> Pit of Saron
#   Deliverance from the Pit  - 24710 Alliance / 24712 Horde -> Halls of Reflection
# leader_only is 0 on every one of those rows, so the requirement falls on each
# member individually. Bots never run the Icecrown quest chain, so not one
# character on the realm held any of the four - the core refused the teleport and
# the party assembler recorded a run into a wing it had never actually entered.
# Measured before this seed: zero rows for all four quests, and no map-668
# instance had ever been created on the realm.
#
# The requirement rows are deliberately left in place so a human player still has
# to earn the attunement the ordinary way; only rndbot% characters are seeded.
# Faction is decided by race rather than by trusting the quest id; the 0/1 column
# below carries the same meaning as dungeon_access_requirements.faction, and each
# pairing was checked against quest_template.AllowableRaces (1101 is the Alliance
# race mask, 690 the Horde one).
#
# Idempotent: INSERT IGNORE against the (guid, quest) primary key. Rewarded
# quests load into the player at login, so the worldserver has to restart (or the
# bots cycle) before the door opens for them.
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/lib.sh

SQL=$(cat <<'EOF'
CREATE TEMPORARY TABLE bot_eighties AS
SELECT c.guid, c.race FROM characters c
JOIN acore_auth.account a ON a.id = c.account
WHERE a.username LIKE 'rndbot%' AND c.level >= 80;

INSERT IGNORE INTO character_queststatus_rewarded (guid, quest, active)
SELECT be.guid, gate.quest, 1
FROM bot_eighties be
JOIN (SELECT 24499 AS quest, 0 AS faction
      UNION ALL SELECT 24710, 0
      UNION ALL SELECT 24511, 1
      UNION ALL SELECT 24712, 1) gate
  ON (gate.faction = 0 AND be.race IN (1,3,4,7,11))
  OR (gate.faction = 1 AND be.race IN (2,5,6,8,10));

SELECT COUNT(*) AS bot_eighties FROM bot_eighties;
SELECT quest, COUNT(*) AS attuned FROM character_queststatus_rewarded
WHERE quest IN (24499,24511,24710,24712) GROUP BY quest;
EOF
)

docker exec ac-database sh -c "mysql -uroot -p\"\$MYSQL_ROOT_PASSWORD\" acore_characters -e \"$SQL\""
echo "[seed-frozen-halls-access] done - restart worldserver so bots load the attunement"
