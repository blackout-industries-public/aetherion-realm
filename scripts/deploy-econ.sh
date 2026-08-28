#!/usr/bin/env bash
# Deploy the economy program to a running realm WITHOUT wiping it: sync the
# repo's patches/scripts, re-apply the chain, rebuild, arm the Econ keys
# persistently (C3: through overlay/.env + configure.sh), restart.
#
# Run FROM the workstation: scripts/deploy-econ.sh <ssh-host>
# Assumes the realm layout at /opt/warcraft on the host.
set -euo pipefail

HOST=${1:?usage: deploy-econ.sh <ssh-host>}
REPO=$(cd "$(dirname "$0")/.." && pwd)

echo "[deploy] syncing repo -> $HOST:/opt/warcraft"
rsync -a "$REPO/patches/" "$HOST:/opt/warcraft/patches/"
rsync -a "$REPO/scripts/" "$HOST:/opt/warcraft/scripts/"
# .env is server-local (DOCKER_DB_ROOT_PASSWORD for compose interpolation) and
# exists in no working copy - a sync that deletes it silently blanks the
# dashboard's DB password. Excluded so even an added --delete cannot take it.
rsync -a --exclude node_modules --exclude .nuxt --exclude .output --exclude .env \
    "$REPO/frontend/" "$HOST:/opt/warcraft/frontend/"
rsync -a --exclude __pycache__ --exclude .venv \
    "$REPO/ai-bridge/" "$HOST:/opt/warcraft/ai-bridge/"
rsync -a "$REPO/overlay/docker-compose.override.yml" "$HOST:/opt/warcraft/overlay/"

echo "[deploy] arming Econ keys in overlay/.env (persistent, C3)"
ssh "$HOST" '
set -e
ENV=/opt/warcraft/overlay/.env
arm() { grep -q "^$1=" "$ENV" || printf "%s=%s\n" "$1" "$2" >> "$ENV"; }
arm ECON_NEEDS 1
arm ECON_EVENTS 1
arm ECON_PREEMPT 1
arm ECON_VENDOR 1
arm ECON_PROTECT_GOODS 1
arm ECON_PAID_REPAIRS 1
arm ECON_PAID_TRAINING 1
arm ECON_AH 1
arm ECON_AH_BUY 1
arm ECON_MAILBOX 1
arm ECON_CRAFT 1
arm ECON_BANK 1
arm ECON_GATHER 1
arm ECON_RARE 1
arm ECON_GEAR_RESCUE 1
arm ECON_GEAR_SHOP 1
arm BOT_CHEATS taxi,raid
cp /opt/warcraft/overlay/docker-compose.override.yml /opt/warcraft/azerothcore/docker-compose.override.yml
'

echo "[deploy] applying patch chain + building worldserver (long)"
ssh "$HOST" '
set -e
/opt/warcraft/patches/llm/apply.sh /opt/warcraft/azerothcore/modules/mod-playerbots
cd /opt/warcraft/azerothcore && docker compose build ac-worldserver
'

echo "[deploy] configure + restart worldserver"
ssh "$HOST" '
set -e
/opt/warcraft/scripts/configure.sh
cd /opt/warcraft/azerothcore && docker compose up -d --force-recreate ac-worldserver
'

# The config directory is a live bind mount, so a key written into the file is
# NOT proof the running process read it - a restart that races configure.sh
# leaves the server on its defaults with the file looking perfect. That silently
# cost a full observation window: every Econ.Gear key sat at 1 on disk while the
# worldserver ran with all of them off. The server says so in its own log; ask it.
echo "[deploy] verifying the server actually read the Econ keys"
ssh "$HOST" '
set -e
sleep 60
ST=$(docker inspect -f "{{.State.StartedAt}}" ac-worldserver)
CONF=/opt/warcraft/config/modules/playerbots.conf
# A key the server calls missing while the file defines it is the race, and the
# only case worth failing on. A key missing from both is just an unset knob
# sitting on its compiled-in default, which is fine and long-standing.
RACED=$(docker logs --since "$ST" ac-worldserver 2>&1 |
    grep -o "Missing property AiPlayerbot[A-Za-z.]*" | awk "{print \$3}" | sort -u |
    while read -r k; do grep -q "^$k *=" "$CONF" && echo "$k"; done)
if [ -n "$RACED" ]; then
    echo "config did not reach the worldserver: $RACED" >&2
    echo "reconfigure and restart again" >&2
    exit 1
fi
echo "every configured key was read by the worldserver"
'

echo "[deploy] frontend + bridge"
ssh "$HOST" '
set -e
cd /opt/warcraft/frontend && docker compose build && docker compose up -d
cd /opt/warcraft/ai-bridge && docker compose build && docker compose up -d --force-recreate
'

echo "[deploy] done - verify: dashboard ECON tab, worldserver health, /metrics"
