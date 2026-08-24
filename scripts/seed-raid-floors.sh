#!/usr/bin/env bash
# Seed per-difficulty average-item-level floors for WotLK raids into
# dungeon_access_template. The core enforces them at the portal
# (DungeonAccessRequirements.PortalAvgIlevelCheck), in the dungeon finder,
# and in the assembler's trip-time access check - one data source, three
# gates, so a green-geared raid never marches on a heroic lockout.
#
# Values are the era's community entry bars (GearScore-age conventions),
# rounded: normal-mode loot level of the previous tier. Idempotent UPDATEs
# keyed by (map_id, difficulty); classic and TBC raids stay at 0 - their
# level floor is the real gate.
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/lib.sh

SQL=$(cat <<'EOF'
UPDATE dungeon_access_template SET min_avg_item_level = 180 WHERE map_id IN (533,615,616,624) AND difficulty = 0;
UPDATE dungeon_access_template SET min_avg_item_level = 187 WHERE map_id IN (533,615,616,624) AND difficulty = 1;
UPDATE dungeon_access_template SET min_avg_item_level = 200 WHERE map_id IN (603,249) AND difficulty = 0;
UPDATE dungeon_access_template SET min_avg_item_level = 213 WHERE map_id IN (603,249) AND difficulty = 1;
UPDATE dungeon_access_template SET min_avg_item_level = 219 WHERE map_id = 649 AND difficulty = 0;
UPDATE dungeon_access_template SET min_avg_item_level = 226 WHERE map_id = 649 AND difficulty = 1;
UPDATE dungeon_access_template SET min_avg_item_level = 232 WHERE map_id = 649 AND difficulty = 2;
UPDATE dungeon_access_template SET min_avg_item_level = 239 WHERE map_id = 649 AND difficulty = 3;
UPDATE dungeon_access_template SET min_avg_item_level = 232 WHERE map_id = 631 AND difficulty = 0;
UPDATE dungeon_access_template SET min_avg_item_level = 245 WHERE map_id = 631 AND difficulty = 1;
UPDATE dungeon_access_template SET min_avg_item_level = 251 WHERE map_id = 631 AND difficulty = 2;
UPDATE dungeon_access_template SET min_avg_item_level = 258 WHERE map_id = 631 AND difficulty = 3;
UPDATE dungeon_access_template SET min_avg_item_level = 245 WHERE map_id = 724 AND difficulty = 0;
UPDATE dungeon_access_template SET min_avg_item_level = 258 WHERE map_id = 724 AND difficulty = 1;
UPDATE dungeon_access_template SET min_avg_item_level = 264 WHERE map_id = 724 AND difficulty = 2;
UPDATE dungeon_access_template SET min_avg_item_level = 271 WHERE map_id = 724 AND difficulty = 3;
UPDATE dungeon_access_template SET min_avg_item_level = 160 WHERE map_id IN (574,575,576,578,595,599,600,601,602,604,608,619) AND difficulty = 1;
UPDATE dungeon_access_template SET min_avg_item_level = 180 WHERE map_id = 650 AND difficulty = 1;
SELECT map_id, difficulty, min_avg_item_level FROM dungeon_access_template WHERE min_avg_item_level > 0 ORDER BY map_id, difficulty;
EOF
)

docker exec ac-database sh -c "mysql -uroot -p\"\$MYSQL_ROOT_PASSWORD\" acore_world -e \"$SQL\""
echo "[seed-raid-floors] done - worldserver reload or restart required to pick up access rows"
