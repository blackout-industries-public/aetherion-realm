#!/usr/bin/env bash
# Logical dump of all four databases plus the config tree.
# Retention per BRD s16: 7 daily, 4 weekly, 3 monthly.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

STAMP=$(date -u +%Y%m%d-%H%M%S)
DEST=$ROOT/backups/daily
mkdir -p "$DEST" "$ROOT/backups"/{weekly,monthly}

# aetherion_ai holds bot memory - relationships and conversation history. It lives
# on the same server precisely so it is covered by this one dump.
DBS="acore_auth acore_characters acore_world acore_playerbots aetherion_ai"
log "dumping: $DBS"
# --single-transaction keeps the world running; all four DBs are InnoDB.
dc exec -T ac-database mysqldump -uroot -p"$DOCKER_DB_ROOT_PASSWORD" \
    --single-transaction --quick --routines --events \
    --databases $DBS | gzip -1 > "$DEST/db-$STAMP.sql.gz"

[[ -s $DEST/db-$STAMP.sql.gz ]] || die "dump is empty - backup FAILED"
tar czf "$DEST/config-$STAMP.tar.gz" -C "$ROOT" config overlay

prune() {
    local dir=$1 keep=$2
    # tail-based instead of mapfile: macOS ships bash 3.2 and this script also
    # runs on the staging laptop.
    # The || true matters twice over: an empty dir makes ls fail, and pipefail
    # would turn that into a script abort.
    (ls -1t "$dir"/$3 2>/dev/null || true) | tail -n +"$((keep + 1))" | while read -r f; do
        rm -f "$f"
    done
    return 0
}
# Promotion, so weekly/ and monthly/ actually hold anything. The prune calls below
# assumed something was putting files there; nothing ever did, which capped real
# retention at 7 days against the BRD's stated 7/4/3.
[[ $(date -u +%u) == 7 ]] && cp "$DEST/db-$STAMP.sql.gz" "$ROOT/backups/weekly/"
[[ $(date -u +%d) == 01 ]] && cp "$DEST/db-$STAMP.sql.gz" "$ROOT/backups/monthly/"

prune "$DEST"                 7 'db-*.sql.gz'
prune "$ROOT/backups/weekly"  4 'db-*.sql.gz'
prune "$ROOT/backups/monthly" 3 'db-*.sql.gz'
prune "$DEST"                 7 'config-*.tar.gz'

log "backup ok: $(du -h "$DEST/db-$STAMP.sql.gz" | cut -f1) -> $DEST/db-$STAMP.sql.gz"
