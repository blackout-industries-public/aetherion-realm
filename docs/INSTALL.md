# Install

From a bare Ubuntu Server VM to a populated realm. Roughly an hour, most of it
unattended compilation.

## Host requirements

| | Minimum | Notes |
|---|---|---|
| OS | Ubuntu Server LTS | Tested on 24.04 and 26.04 |
| vCPU | 8 | Fast cores matter more than core count |
| RAM | **16 GB** | 8 GB runs ~50 bots; 1500 bots needs 16 GB |
| Disk | 150 GB SSD-backed | Client data alone is ~3 GB extracted |
| Network | Bridged LAN, **static or reserved IP** | A moving IP breaks every client's realmlist |

Docker Engine and the Compose plugin. Nothing else - the build happens in containers.

```bash
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo tee /etc/apt/keyrings/docker.asc >/dev/null
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo $VERSION_CODENAME) stable" \
  | sudo tee /etc/apt/sources.list.d/docker.list >/dev/null
sudo apt-get update && sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin git
sudo usermod -aG docker "$USER"   # log out and back in
```

## 1. Clone and pin

```bash
sudo mkdir -p /opt/warcraft && sudo chown "$(id -u):$(id -g)" /opt/warcraft
git clone <this repo> /opt/warcraft && cd /opt/warcraft
./scripts/bootstrap.sh
```

`bootstrap.sh` clones AzerothCore's Playerbots fork and the modules at the exact
commits in `overlay/.env`, generates a `.env` with a random database password,
applies the patches in `patches/llm/`, and stages the compose override.

It is idempotent. Re-run it after editing pins, and it re-pins and re-patches.

## 2. Build

```bash
cd azerothcore && docker compose build
```

Around 40 minutes on 8 cores. `BUILD_JOBS` in `overlay/.env` caps compile
parallelism - each clang job peaks near 1.5 GB, so keep `jobs x 1.5 GB` under your
RAM or the build is OOM-killed part-way and leaves a poisoned ccache.

## 3. First start

```bash
docker compose up -d
```

This downloads ~1.2 GB of client data (maps, vmaps, mmaps, DBC) from
`wowgaming/client-data`, imports four databases, and starts the realm. The first
`up` takes several minutes; the world server needs a few minutes more to load maps.

**No Blizzard files are needed and none are distributed.**

## 4. Configure

```bash
cd /opt/warcraft && ./scripts/configure.sh
```

Applies realm settings, bot population, level brackets, the economy and - if you want
it - the AI layer. Every value is overridable by environment variable; see
[OPERATIONS.md](OPERATIONS.md).

Restart the world server afterwards so it picks the changes up:

```bash
cd azerothcore && docker compose restart ac-worldserver
```

## 5. Accounts

The world server console needs an interactive TTY, so account creation goes through
a script that reproduces the core's own SRP6 derivation:

```bash
./scripts/create-account.sh admin '<password>' 3 -1   # 3 = admin, -1 = all realms
./scripts/setup-ahbot.sh                              # market identity for the auction house
./scripts/configure.sh
```

## 6. Verify

```bash
./scripts/smoke.sh    # 13 checks; exit code is the failure count
./scripts/status.sh   # population, zones, database size, tick performance
```

## 7. Connect

Point `realmlist.wtf` in a 3.3.5a client at the host:

```
set realmlist 10.0.0.x
```

The file exists in both `<WoW>\realmlist.wtf` and `<WoW>\Data\enUS\realmlist.wtf`;
the locale one wins. Set it read-only afterwards or the launcher rewrites it.

## 8. Autostart and backups

```bash
sudo cp ops/systemd/*.service ops/systemd/*.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now warcraft.service warcraft-backup.timer
```

Daily dumps at 04:30 UTC with 7/4/3 retention. Verify one restores before trusting it:

```bash
./scripts/verify-restore.sh   # loads the newest dump into a scratch instance
```

## Optional: the AI layer

Requires an OpenAI-compatible endpoint (LM Studio, Ollama, vLLM, or a gateway such
as Bifrost in front of them).

```bash
cd ai-bridge && docker compose up -d --build
curl -s localhost:8090/health
cd /opt/warcraft && LLM_ENABLED=1 ./scripts/configure.sh
cd azerothcore && docker compose restart ac-worldserver
```

Model choice is not free-form - most local models return empty replies because
reasoning consumes the whole token budget. Read `ai-bridge/README.md` before picking.

## Optional: the dashboard

```bash
cd frontend && docker compose up -d --build   # http://<host>:3000
```

Map art is generated from a client **you own**; see `frontend/maps/README.md`.
