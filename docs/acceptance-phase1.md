# Phase 1 Acceptance Criteria (BRD s18)

Phase 2 does not start until every mandatory row is `PASS`.
Status legend: PASS / FAIL / PENDING / N-A.

## Infrastructure

| # | Criterion | Status | Evidence |
|---|---|---|---|
| 1 | Ubuntu VM stable | PASS | Ubuntu 26.04 LTS, kernel 7.0.0-30 |
| 2 | Static/reserved LAN address | PENDING | 10.10.25.193 is a DHCP lease; needs a reservation |
| 3 | SSH administration available | PASS | key auth, passwordless sudo |
| 4 | Time synchronization configured | PASS | `timedatectl` NTPSynchronized=yes, TZ Etc/UTC |
| 5 | Firewall configured | PASS | ufw default-deny; 22/3724/8085 open, 3306/7878 loopback-only |
| 6 | Automatic service startup | PASS | `warcraft.service` enabled |
| 7 | Disk sized per BRD (150 GB+) | PASS | root LV extended 100 GB -> 244 GB |
| 8 | RAM per BRD (16 GB min) | FAIL | host has 7.3 GB; Proxmox-side change required |

## AzerothCore

| # | Criterion | Status | Evidence |
|---|---|---|---|
| 9 | Auth server operational | PASS | container healthy, TCP 3724 accepts |
| 10 | World server operational | PASS | healthy, 0 restarts; tick diff mean 6 ms, p95 19 ms, p99 22 ms |
| 11 | 3.3.5a client connects | **NEEDS CLIENT** | realmlist row correct; requires a human with the 3.3.5a client |
| 12 | Account creation validated | PASS | `admin` created via SRP6; the core itself authenticated it over RA |
| 13 | Character creation validated | **NEEDS CLIENT** | 1001 bot characters exist; human character creation needs the client |
| 14 | GM account validated | PASS | gmlevel 3 accepted by worldserver RA auth |

## Playerbots

| # | Criterion | Status | Evidence |
|---|---|---|---|
| 15 | Compiles against pinned Playerbot core | PASS | 1837 objects, clean; mod-ah-bot also compiles against the fork |
| 16 | Random bots populate world | PASS | 50 online of a 1001-character pool, across maps 0/1/530/571 |
| 17 | Bots level | PASS | sum(level) 25276 -> 25314 over 5 min with no human online |
| 18 | Bots quest | PASS | character_queststatus turns over between samples |
| 19 | Bots travel | PASS | population spread over 4 maps and ~15 zones concurrently |
| 20 | Bots fight | PASS | +2,890 gold earned across the population in 5 min |
| 21 | Human can group with bots | **NEEDS CLIENT** | |
| 22 | Representative dungeon completed | **NEEDS CLIENT** | see `dungeon-raid-matrix.md` |
| 23 | Bot population survives restart | PASS | identical counts across a full VM reboot |

## Economy

| # | Criterion | Status | Evidence |
|---|---|---|---|
| 24 | AH contains plausible inventory | PASS | 741 listings; grey/white/green/blue/purple pools loaded |
| 25 | AH bot sells | PASS | AHBot [1001] update cycles posting listings (450 -> 741) |
| 26 | AH bot purchases qualifying human listings | **NEEDS CLIENT** | buyer enabled; needs a human listing to bid against |
| 27 | Pricing does not destroy progression | **OBSERVE** | bots hold ~656g average at init; watch before raising ItemsPerCycle |
| 28 | AH survives restart | PASS | 741 listings identical across reboot |

## Operations

| # | Criterion | Status | Evidence |
|---|---|---|---|
| 29 | Automated backup succeeds | PASS | 111 MB dump in 5 s; timer armed for 04:30 UTC |
| 30 | Backup restore tested | PASS | `verify-restore.sh` loads the dump into a scratch MySQL; table counts match all 4 schemas |
| 31 | Server reboot tested | PASS | full VM reboot; all state identical afterwards |
| 32 | Services recover automatically | PASS | all 3 containers healthy ~3 min after boot with no intervention |
| 33 | Logs accessible | PASS | `docker compose logs`, /opt/warcraft/logs, json-file capped 20 MB x5 |
| 34 | Baseline CPU/RAM/DB metrics captured | PASS | see Baseline below |

## Baseline (50 bots, no human online)

| Metric | Value |
|---|---|
| Worldserver tick diff | mean 6 ms, median 1 ms, p95 19 ms, p99 22 ms, max 25 ms |
| Host RAM | 5.7 GB used of 7.3 GB |
| Host load (8 vCPU) | ~1.0 |
| Disk | 29 GB used of 244 GB |
| DB size | world 434 MB, playerbots 74 MB, characters 39 MB, auth 1 MB |
| Backup | 111 MB gzip, 5 s |
| Bot pool | 1001 characters, 50 online, 100 per class across all 10 classes |
| Guilds | 20 formed by bots |

RAM is the binding constraint: 5.7 of 7.3 GB at only 50 bots. Stage B (100-200)
is plausible; Stage C/D is not reachable until the VM is given more memory.

## Remaining items

Six criteria need a human at a 3.3.5a client (11, 13, 21, 22, 26) or a period of
observation (27). Everything reachable from the server side passes.
