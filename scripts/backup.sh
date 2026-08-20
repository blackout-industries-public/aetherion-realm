#!/usr/bin/env bash
# Logical dump of all four databases plus the config tree.
# Retention per BRD s16: 7 daily, 4 weekly, 3 monthly.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

STAMP=$(date -u +%Y%m%d-%H%M%S)
DEST=$ROOT/backups/daily
mkdir -p "$DEST" "$ROOT/backups"/{weekly,monthly}

DBS="acore_auth acore_characters acore_world acore_playerbots"
log "dumping: $DBS"
# --single-transaction keeps the world running; all four DBs are InnoDB.
dc exec -T ac-database mysqldump -uroot -p"$DOCKER_DB_ROOT_PASSWORD" \
    --single-transaction --quick --routines --events \
    --databases $DBS | gzip -1 > "$DEST/db-$STAMP.sql.gz"

[[ -s $DEST/db-$STAMP.sql.gz ]] || die "dump is empty - backup FAILED"
tar czf "$DEST/config-$STAMP.tar.gz" -C "$ROOT" config overlay

# Promote by calendar position rather than copying on every run.
# Written as `if`, not `[[ ]] && cp`: under `set -e` a false AND-list is itself a
# failing command and would abort the script on every non-Sunday, silently skipping
# retention pruning.
if [[ $(date -u +%u) == 7 ]]; then cp "$DEST/db-$STAMP.sql.gz" "$ROOT/backups/weekly/"; fi
if [[ $(date -u +%d) == 01 ]]; then cp "$DEST/db-$STAMP.sql.gz" "$ROOT/backups/monthly/"; fi

# `ls` exits 2 when the glob matches nothing, and lib.sh sets pipefail, so the
# obvious `ls | tail | xargs` pipeline aborts the whole script the first time a
# retention directory is empty - after the dump is written but before pruning.
prune() {
    local dir=$1 keep=$2
    local -a files=()
    mapfile -t files < <(ls -1t "$dir"/$3 2>/dev/null || true)
    (( ${#files[@]} > keep )) && rm -f "${files[@]:keep}"
    return 0
}
prune "$DEST"                 7 'db-*.sql.gz'
prune "$ROOT/backups/weekly"  4 'db-*.sql.gz'
prune "$ROOT/backups/monthly" 3 'db-*.sql.gz'
prune "$DEST"                 7 'config-*.tar.gz'

log "backup ok: $(du -h "$DEST/db-$STAMP.sql.gz" | cut -f1) -> $DEST/db-$STAMP.sql.gz"
