#!/usr/bin/env bash
# Prove a dump is actually restorable (BRD s16: an untested backup is not a backup).
#
# Loads the newest dump into a disposable MySQL container and compares table counts
# against the live server. The live databases are never touched, so this is safe to
# run on a schedule.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

DUMP=${1:-$(ls -1t "$ROOT"/backups/daily/db-*.sql.gz 2>/dev/null | head -1)}
[[ -f $DUMP ]] || die "no dump found"
log "verifying $DUMP ($(du -h "$DUMP" | cut -f1))"

gunzip -t "$DUMP" || die "gzip integrity check failed"

NAME=ac-restore-verify
TESTPW=$(openssl rand -hex 16)
cleanup() { docker rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup

# Sized small on purpose: this runs alongside the live server on a 7 GB host.
docker run -d --rm --name "$NAME" -e MYSQL_ROOT_PASSWORD="$TESTPW" mysql:8.4 \
    --innodb-buffer-pool-size=128M --performance-schema=OFF >/dev/null

# Gate on an authenticated query, not `mysqladmin ping`: ping answers during the
# image's temporary init server, before the root password has been applied, so a
# ping-based wait races straight into "Access denied".
log "waiting for the scratch instance"
ready=0
for _ in $(seq 1 90); do
    if docker exec "$NAME" mysql -uroot -p"$TESTPW" -e "SELECT 1" >/dev/null 2>&1; then ready=1; break; fi
    sleep 2
done
(( ready )) || die "scratch instance never became ready"

log "restoring"
gunzip -c "$DUMP" | docker exec -i "$NAME" mysql -uroot -p"$TESTPW" 2>/dev/null \
    || die "restore FAILED"

counts() { # counts <exec-prefix...>
    "$@" -N -B -e "SELECT table_schema, COUNT(*) FROM information_schema.tables
                   WHERE table_schema LIKE 'acore%' GROUP BY table_schema ORDER BY 1;" 2>/dev/null
}
LIVE=$(counts dc exec -T ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD")
REST=$(counts docker exec -i "$NAME" mysql -uroot -p"$TESTPW")

printf '\nlive:\n%s\n\nrestored:\n%s\n\n' "$LIVE" "$REST"
if [[ $LIVE == "$REST" ]]; then
    log "RESTORE VERIFIED - table counts match across all databases"
else
    die "RESTORE MISMATCH - see the two listings above"
fi
