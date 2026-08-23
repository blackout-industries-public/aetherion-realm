#!/usr/bin/env bash
# Server wipe + race start: every character is destroyed and recreated at level 1
# with nothing, natural leveling only, and the RACE tab clock starts.
#
# DESTRUCTIVE AND IRREVERSIBLE except through the backup taken by this script.
# Guarded three ways: an explicit flag, the exact-target-name confirmation, and a
# verified fresh backup before anything is dropped.
#
# Prerequisites (validated below): the worldserver image must already be built
# from a source tree containing patch_racemode.py, because race mode's one-time
# init lives in C++. The patch is inert until RACE_MODE flips the config, so it
# is safe to deploy any time before the wipe.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

TARGETS="acore_characters acore_playerbots aetherion_ai"

[[ ${1:-} == --i-understand-data-loss ]] \
    || die "refusing: run as wipe-race-start.sh --i-understand-data-loss [race label]"
RACE_LABEL=${2:-"Race $(date -u +%Y-%m-%d)"}

MODULE=$AC/modules/mod-playerbots
grep -q "Race mode:" "$MODULE/src/Bot/RandomPlayerbotMgr.cpp" \
    || die "module source lacks patch_racemode; run patches/llm/apply.sh and rebuild first"

mysql_root() { dc exec -T ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD" "$@"; }

CHARS=$(mysql_root -N -e "SELECT COUNT(*) FROM acore_characters.characters" 2>/dev/null || echo "?")
LATEST_BACKUP=$(ls -t "$ROOT"/backups/daily/db-*.sql.gz 2>/dev/null | head -1 || true)

cat >&2 <<BLOCK

🚨🚨🚨 DATA LOSS WARNING 🚨🚨🚨
⚠️  Operation:   DROP DATABASE acore_characters, acore_playerbots; TRUNCATE every
                aetherion_ai table and acore_auth.realmcharacters
⚠️  Target:      ${TARGETS} at ${ROOT} on $(hostname)
⚠️  Effect:      IRREVERSIBLE - all ${CHARS} characters, their items, gold, guilds,
                quests, auctions, mail, and all recorded history/milestones
⚠️  Backup:      ${LATEST_BACKUP:-NO BACKUP FOUND} (a fresh one is taken before the drop)
⚠️  Blast radius: dashboard shows an empty realm until bots are recreated; LLM
                relationship memory references dead guids and is cleared with it
🚨 Reply with the exact target names to proceed, or anything else to abort.

BLOCK

if [[ -n ${WIPE_CONFIRM_TARGETS:-} ]]; then
    REPLY=$WIPE_CONFIRM_TARGETS
else
    read -r -p "targets> " REPLY
fi
[[ $REPLY == "$TARGETS" ]] || die "confirmation mismatch; aborted, nothing touched"

log "1/8 fresh backup"
"$ROOT/scripts/backup.sh"
FRESH=$(ls -t "$ROOT"/backups/daily/db-*.sql.gz | head -1)
gzip -t "$FRESH" || die "backup failed integrity check; aborted, nothing touched"
log "backup verified: $FRESH ($(du -h "$FRESH" | cut -f1))"

log "2/8 stopping worldserver and recorder"
dc stop ac-worldserver
docker compose --project-directory "$ROOT/ai-bridge" stop

log "3/8 dropping and recreating databases"
mysql_root <<'SQL'
DROP DATABASE IF EXISTS acore_characters;
DROP DATABASE IF EXISTS acore_playerbots;
CREATE DATABASE acore_characters DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE DATABASE acore_playerbots DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
TRUNCATE acore_auth.realmcharacters;
SQL

log "4/8 re-importing base schema (db-import)"
docker start ac-db-import >/dev/null
RC=$(docker wait ac-db-import)
[[ $RC == 0 ]] || die "db-import exited $RC; databases are empty - restore from $FRESH"
TABLES=$(mysql_root -N -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='acore_characters'")
[[ $TABLES -gt 50 ]] || die "characters schema import produced only $TABLES tables - restore from $FRESH"

log "5/8 resetting recorder history and starting the race clock"
for t in $(mysql_root -N -e "SHOW TABLES FROM aetherion_ai"); do
    mysql_root -e "TRUNCATE aetherion_ai.\`$t\`"
done
mysql_root <<SQL
CREATE TABLE IF NOT EXISTS aetherion_ai.milestones (
    id      BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    ts      DOUBLE          NOT NULL,
    kind    VARCHAR(24)     NOT NULL,
    detail  VARCHAR(160)    NOT NULL,
    guid    INT UNSIGNED    NULL,
    who     VARCHAR(120)    NOT NULL DEFAULT '',
    PRIMARY KEY (id),
    UNIQUE KEY first_only (kind, detail)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
INSERT INTO aetherion_ai.milestones (ts, kind, detail, guid, who)
VALUES (UNIX_TIMESTAMP(), 'race_start', '$RACE_LABEL', NULL, '');
SQL

log "6/8 arming race mode in overlay/.env and applying config"
if grep -q '^RACE_MODE=' "$ROOT/overlay/.env"; then
    sed -i 's/^RACE_MODE=.*/RACE_MODE=1/' "$ROOT/overlay/.env"
else
    printf 'RACE_MODE=1\n' >> "$ROOT/overlay/.env"
fi
RACE_MODE=1 "$ROOT/scripts/configure.sh"

log "7/8 starting worldserver (recreates all bots at level 1 - this takes a while)"
dc up -d ac-worldserver
PREV=-1; STABLE=0
for _ in $(seq 1 90); do
    sleep 30
    N=$(mysql_root -N -e "SELECT COUNT(*) FROM acore_characters.characters" 2>/dev/null || echo 0)
    log "  characters created: $N"
    if [[ $N -gt 0 && $N == "$PREV" ]]; then
        STABLE=$((STABLE + 1))
        [[ $STABLE -ge 4 ]] && break
    else
        STABLE=0
    fi
    PREV=$N
done

log "8/8 restarting recorder with a clean baseline"
docker compose --project-directory "$ROOT/ai-bridge" up -d --force-recreate

log "race started: '$RACE_LABEL'. Watch the RACE tab; rollback = scripts/restore.sh $FRESH"
