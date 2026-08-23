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
rsync -a --exclude node_modules --exclude .nuxt --exclude .output \
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
cd /opt/warcraft/azerothcore && docker compose up -d ac-worldserver
'

echo "[deploy] frontend + bridge"
ssh "$HOST" '
set -e
cd /opt/warcraft/frontend && docker compose build && docker compose up -d
cd /opt/warcraft/ai-bridge && docker compose build && docker compose up -d --force-recreate
'

echo "[deploy] done - verify: dashboard ECON tab, worldserver health, /metrics"
