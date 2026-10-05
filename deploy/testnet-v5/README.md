# Fresh public v5 testnet — 2026-10-05

This release starts **fresh genesis**, chain `2094b0868a27b032`. It does not
resume the saved ASUS experiment at share 5200 or reuse any v3 volume, wallet,
peer table or database. Keep those histories and identities archived separately.
The public Explorer remains `https://explorer.catasterism.xyz`; Apache continues
proxying host loopback port 3071. Cloud P2P continues on port 7043.

Craig alone pushes Git and runs production commands. The commands below are for
the dedicated Lightsail host, from the reviewed repository checkout. Homelab
work may be performed by the agent; Wolf359 requires Craig's sudo.

## Build and qualification

Default repository builds still select v3. Use the explicit v5 Dockerfiles in
`deploy/testnet-v5-asus/`. The node build runs the full default C/integration and
Python suites, then compiles v5 and enforces the 196608-byte limit. Explorer runs
the v5 Go tests before its static build. ARM uses 4 KiB segment alignment and
disables unused C unwind tables; stack protection, FORTIFY, RELRO and the
non-executable stack remain. This ARM build is qualified for Mini's 4 KiB-page
Colima Linux guest.

The new temperature publisher handles SIGTERM as PID 1 and removes its sample
on shutdown. Invalid/missing input removes the sample too. Mini's existing real
macmon publisher remains the source; the v5 helper rejects samples older than
five seconds and publishes into a separate directory with its own mining hold.
Linux helpers read actual k10temp (ASUS) or coretemp (NixOS) package readings.

Local checks (no production access):

```sh
python3 -B deploy/testnet-v5/test_temperature.py
python3 -B deploy/testnet-v5/test_cloud.py
python3 -B deploy/testnet-v5/test_cutover.py
```

The last test exercises the actual cutover script with disposable local projects:
private image/data backup, a real Postgres restore with matching digests for all
tables, fresh v5 genesis, refusal to reuse existing v5 volumes, and rollback to
the exact v3 tip and ledger. It uses the retained local v3 Lightsail test images.
The cloud-profile test checks resource limits, private database networking,
loopback HTTP, validation-only mode, persistent identity and independent restarts.
Cross-host rehearsal evidence and final image identities are recorded separately
in `RELEASE.md` when qualification is complete.

## Target inventory

| Host | Project / namespace | Data | Initial role |
| --- | --- | --- | --- |
| Lightsail | `constella-cloud-v5` | `node-data`, `pgdata` (project-prefixed) | Validator + Explorer + Postgres |
| ASUS | `constella-public-v5-asus` | `data` (project-prefixed) | Two workers, duty 25%, 1 CPU / 1 GiB, cap80 |
| Mini | `constella-public-v5-mini` | `data` (project-prefixed) | Two workers, duty 25%, 1 CPU / 1 GiB, cap80 |
| NixOS | `constella-public-v5-nixos` | `data` (project-prefixed) | Two workers, duty 25%, 1 CPU / 1 GiB, cap88 |
| Wolf359 | `constella-testnet-v5` | New 20 GiB retained PVC | Validator, 1 CPU / 1 GiB |

Homelab listeners bind their LAN IP on port 18473. Their explicit peer lists
include the cloud and other homelab nodes. The cloud has no external seed and
receives outbound homelab connections; v5 has no built-in DNS seed. The cloud
has mining disabled and creates no spending wallet or thermal sampling thread.
Postgres has no host port and shares no network with the node.

All initial miner starts require `thermal/mining_cpu_millidegrees` to be absent.
The hold is manual, not an automatic synchronization gate. A real temperature
file by itself does not release mining. Do not import rehearsal keys/history.

## 1. Craig: prepare cloud images and private configuration

After pushing the reviewed release, start from the existing cloud repository
directory. Its history predates the agent-file removal, so use a fresh worktree
instead of trying to fast-forward or reset the old checkout:

```sh
legacy_checkout=$PWD
git fetch origin
git worktree add --detach ../constella-v5-20261005 origin/master
release_checkout=$(cd ../constella-v5-20261005 && pwd)
cmp "$legacy_checkout/deploy/lightsail/compose.yml" "$release_checkout/deploy/lightsail/compose.yml"
sudo install -m 600 "$legacy_checkout/deploy/lightsail/production.env" "$release_checkout/deploy/lightsail/production.env"
cd "$release_checkout"
```

If the Compose comparison differs, stop and inspect the existing host changes
before proceeding; the rollback must preserve the actual v3 configuration.
The original checkout and its private configuration remain untouched. Then:

```sh
git rev-parse HEAD
sudo sh deploy/lightsail/preflight.sh
sudo sh deploy/testnet-v5/prepare-cloud.sh
```

This builds/tests the explicit v5 node and Explorer, pins their image IDs and the
installed Postgres image in a **new**, mode-0600 `cloud.env`, and generates a new
database password. It does not stop v3. It refuses to overwrite an existing env.
Do not publish that file or expanded Compose configuration. Cloud resources are
node 0.5 CPU/384 MiB, Explorer 0.5 CPU/640 MiB, Postgres 0.25 CPU/384 MiB.

## 2. Coordinate the v3 checkpoint

Have the agent hold and stop the homelab v3 miners, recording their last tips and
preserving their original volumes, images, private wallet/identity backups and
startup configuration. Keep Mini's real temperature publisher running. Its old
automatic runtime launcher must be switched to the v5 profile at cutover so a
reboot cannot restart the old miner. Wolf359's old validator can remain up until
its separate operator step below.

Craig then runs, on Lightsail:

```sh
sudo sh deploy/testnet-v5/cloud-cutover.sh backup
```

The private backup directory is `/var/backups/constella/v3-before-v5-20261005`.
The script verifies it sees v3, checks disk headroom, saves the original config
and exact images, stops only the v3 node/Explorer, captures a Postgres custom dump
and stopped-node data archive, and records SHA-256 checksums. The node archive
contains its private identity; keep it private. Existing volumes stay intact.
The v3 database remains up until the next step. A failed/partial backup has no
`COMPLETE` receipt and cannot authorize the scripted cutover.

## 3. Craig: start fresh cloud v5

```sh
sudo sh deploy/testnet-v5/cloud-cutover.sh start
sudo sh deploy/testnet-v5/cloud-cutover.sh status
python3 deploy/testnet-v5/verify.py http://127.0.0.1:3071 --genesis
```

Expected: chain `2094b0868a27b032`, height 0, zero transactions; node logs show
validation-only mode and zero workers. All v5 volumes must be new. This script
refuses any pre-existing v5 volume; deliberate recovery uses targeted `start`
commands after inspection, never deletion to bypass the guard. At genesis there
are no accounts to cross-check yet. A quiet chain may report stale health after
five minutes; it should become fresh once the first miner starts.

Send the agent the genesis result so it can join the homelab nodes and release
one miner. Then verify public HTTPS and ledger agreement:

```sh
python3 deploy/testnet-v5/verify.py https://explorer.catasterism.xyz
curl --fail --silent --show-error https://explorer.catasterism.xyz/healthz
```

Expected after mining: advancing height, `peer=true`, fresh `check=ok` for all
accounts. Hold miners and add `--exact` for a fixed-height comparison. Exact
account queries and stopped-history replays provide stronger evidence than
comparing asynchronously sampled log heights.

## 4. Homelab rollout and holds

Use `/home/cd/constella-public-v5` on ASUS/NixOS and
`/Users/cd/constella-public-v5` on Mini. Each directory holds `compose.yml` copied
from `compose.node.yml`, a private `.env` from its host example, and new `thermal`
and `sensor` directories owned by the host user. Validate UID/GID and image IDs
against the qualification manifest. Mini uses its existing
`docker --context colima-constella-gate`; its real sensor service stays enabled.

Start temperature and node by service name with the mining alias absent. Verify
the v5 chain ID, peers, zero orphans, real temperature freshness, and convergence.
Enable one miner first. After the Explorer's account check passes, enable the
other miners one at a time, checking the same conditions after each release.

Linux commands, from the new host directory (add Mini's Docker context there):

```sh
docker compose config --quiet
docker compose up -d --no-deps --no-build --pull never temperature node
docker compose logs --tail 30 node
# Release only after convergence and a fresh real sensor sample:
ln -s cpu_millidegrees thermal/mining_cpu_millidegrees
# Hold before maintenance or after synchronization loss:
rm -f thermal/mining_cpu_millidegrees
```

Never use a synthetic sensor reading for a live miner. Monitor actual temperature,
accepted shares, ledger checks, orphans, restarts and memory growth. Start NixOS
at the same controlled 25% duty; raising its duty is a later tuning decision.

## 5. Craig: Wolf359

The staged directory is `/home/cd/constella-wolf359-v5-20261005`, containing the
tested AMD64 image archive/checksum, `wolf359.yml`, `verify.py` and startup script.
Once the public v5 ledger check passes, run on Wolf359:

```sh
cd /home/cd/constella-wolf359-v5-20261005
sudo sh wolf359-start.sh
sudo k3s kubectl -n constella-testnet-v5 get pods,pvc -o wide
sudo k3s kubectl -n constella-testnet-v5 logs node8-0 --tail=30
```

The script checks public v5, verifies/imports the image, stops only the old node8,
archives its retained PVC data privately, and creates a separate namespace/PVC.
Mining stays disabled. Readiness checks only the listener; confirm convergence
from logs and authenticated account queries. Root-only K3s credentials stay
root-only. Other applications and namespaces are outside this change.

## Rollback

Hold and stop the v5 homelab miners before restoring v3. Retain every v5 volume
and log. Craig restores the cloud with:

```sh
sudo sh deploy/testnet-v5/cloud-cutover.sh rollback
curl --fail --silent --show-error http://127.0.0.1:3071/api/stats
```

The script checks backup hashes, stops only v5, reloads the exact saved images and
starts v3 from its original volumes/config. It does not overwrite either chain.
Expected chain is `a8f4562e57e74f9d`; the saved tip may advance when old homelab
peers reconnect. Restore the saved Mini runtime launcher before a v3 reboot.
Restart ASUS node4, Mini node6 and NixOS node7 with their original configuration,
initially held, and verify convergence before releasing old mining holds.
For Wolf359, from its staged directory:

```sh
sudo sh wolf359-start.sh rollback
```

If backup failed before producing `COMPLETE`, the original v3 containers and
volumes remain. After investigating the failure, restart their existing node,
database and Explorer containers; do not delete volumes or treat an incomplete
backup as a restore source.

## Operational checks

Keep at least 20% disk headroom and monitor both Docker data and the backup
filesystem. Check container memory/restarts, node orphan queues and temperatures,
Explorer check age, Postgres errors and Lightsail CPU burst balance. Retain the
private pre-cutover backup on the host and make a private off-host copy through
Craig's normal backup channel. Periodically rehearse database and chain recovery.
Do not use broad Docker prune or remove retained v3/experimental/rehearsal volumes.
The cloud build and initial deployment checks do not establish sustained capacity.
