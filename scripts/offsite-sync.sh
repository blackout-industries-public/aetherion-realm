#!/usr/bin/env bash
# Off-host half of the backup story: push the dumps to the Garage bucket on
# nas02. The realm host's RAM is provably flipping bits, so copies that live
# only on its own disk protect against nothing that matters.
# rclone runs from its container: the host has no root for package installs,
# and build/run-in-Docker is house law anyway. Credentials live in
# overlay/garage.env, never in git and never on a command line.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack

BUCKET=warcraft-db-backups
RCLONE=(docker run --rm --env-file "$ROOT/overlay/garage.env"
        -v "$ROOT/overlay/rclone.conf:/config/rclone/rclone.conf:ro"
        -v "$ROOT/backups:/backups:ro" rclone/rclone:latest)

for tier in daily weekly monthly; do
    [[ -d $ROOT/backups/$tier ]] || continue
    "${RCLONE[@]}" copy "/backups/$tier" "garage:$BUCKET/$tier" || die "push of $tier FAILED"
done

# Read-back proof, not just a clean exit: a copy that cannot be re-read from
# the NAS is the same as no copy.
"${RCLONE[@]}" check /backups/daily "garage:$BUCKET/daily" --one-way \
    || die "remote daily copies do not match local"

# Remote generations outlive the local ones on purpose - the NAS has the room,
# and history is the defence against a corruption nobody noticed for a week.
"${RCLONE[@]}" delete "garage:$BUCKET/daily"   --min-age 30d  || true
"${RCLONE[@]}" delete "garage:$BUCKET/weekly"  --min-age 150d || true
"${RCLONE[@]}" delete "garage:$BUCKET/monthly" --min-age 400d || true

log "offsite ok: $("${RCLONE[@]}" size "garage:$BUCKET" 2>/dev/null | tr '\n' ' ')"
