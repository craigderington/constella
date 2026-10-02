# Homelab node7 on nixos

Craig authorized starting this node on 2026-10-01. It runs independently of
the cloud stack and mini; no production commands are executed by agents.
Host `nixos` is x86_64, four logical CPUs, 31 GiB RAM and 843 GiB free root
disk at provisioning. Clock synchronization was confirmed. Existing unrelated
containers were left unchanged.

Current mode: mining enabled, two workers, **100% duty cap**, one-CPU container
quota and 1 GiB memory limit. Craig requested the increase from 75% on
2026-10-02. The duty cap does not override the aggregate one-CPU quota.

Deployment: `/home/cd/constella-homelab/compose.yml` on nixos.
Project: `constella-gate-nixos`; container: `constella-gate-nixos-node7-1`;
data volume: `constella-gate-nixos_node7`. No host ports are published.
Explicit outbound peers are Lightsail `3.150.62.26:7043` and mini
`192.168.1.123:17046`; other private-address discovery remains disabled.
No host firewall or NixOS system configuration was changed.

The initial tested amd64 node image was transferred directly from local
Docker and loaded on nixos, without a registry push or rebuild. Both sides
reported image ID
`sha256:cca575a931ea3f1d46376d6d619ef466705f8ca6a3d8f7a69194d5ca43b9db51`
under `constella:lightsail-testnet-20261001`. This image can mine; the cloud
deployment disables mining through its explicit environment configuration.

Node7 initially used two workers, 50% duty, a one-CPU container quota, 1 GiB memory cap
and the real `x86_pkg_temp` sensor. Target is 82 C, configured cap 88 C,
with battery/sensor safety retained. On initial join it reported 48 C, two
authenticated peers, zero orphans and height 334. Catch-up is still in progress;
that observation is not a completed ledger/convergence or sustained-capacity test.

Mining was then disabled with `CONSTELLA_MINE=0` for initial synchronization:
the first startup produced a few low-height side-branch shares while downloading
history. The existing wallet volume is retained. Enable mining only after node7
matches the retained chain and its ledger has been checked; the two-worker
thermal settings above apply when mining is enabled. Validation-only mode has
no mining or thermal-sampler threads.

## Controlled mining restart, 2026-10-02 UTC

Craig authorized restarting mining and prioritized nixos. Before replacement,
node7 repeatedly reported height 71,360, tip prefix `3e1424d8`, two peers and
zero orphans, matching the paused network checkpoint. The mini account query
returned `18335.07203993`, nonce 0, height 71,360. The real CPU sensor read 46 C.

The replacement image is `constella:burst-fix-amd64-20261002`, transferred from
the locally tested Docker image without a registry push. Its image ID is:

`sha256:4bbf359cb4d8d8a9dba43266a2045f3b107b1f59258c9f854f0c13fa23cfd10e`

This includes commits `3ebb778` and `e90f48b`; see the mini README for the full
regression, saved-history replay and isolated ARM canary results. This is the
compatible v3 repair, not the opt-in v4 protocol candidate.

The first repaired-image restart enabled mining with two workers and 25% duty.
The existing one-CPU quota, 1 GiB memory limit and real thermal protection are
retained. The mini stays paused while this first connected miner is observed.
The old container stopped cleanly with exit 0, no OOM, and the replacement
started at 02:25:07 UTC using the same named data volume. Its existing wallet
address was verified without exporting keys. Full proof replay precedes mining.

The host retains the previous paused Compose configuration and a public-only
chain copy under `/home/cd/constella-homelab/incident-20261002-restart/`.
That checkpoint contains 218,757 records, SHA-256
`88071f255f3f9b249e12b1f521d3e39f70563fc4ea433a5e88e18c88b6c5f8b0`.
Generated evidence stays outside Git.

Replay completed at 02:56:09 UTC, after 1,862 seconds on this i3-7100U host
with one CPU available. Mining then began automatically. A public-file capture
at 02:58:18 retained the complete original byte prefix and contained 236 new
records, all consecutive parent/child extensions from height 71,360 through
71,596, with no sibling records or partial tail. Its final tip was
`d46148e1a01fe2d012d23df6dfa7b5e6f0728bfe7cfcd3aad40680953a8b4655`.
The analysis checks structure; the running node validates each proof normally.
Difficulty rose from 64 to 256 in that sample. Status reached 71,697 at
03:02:39 with zero orphans and 25% duty; CPU temperature settled to 56–58 C.
The node crossed science boundaries 71,425 and 71,681, had no local-share
rejections/fatal errors/restarts/OOM, and used roughly 57 MiB RAM. Peer count
was one or two during these samples. This is an initial connected observation,
not an overnight stability or long-term capacity result.

The old mini image received these shares but its recovery work lagged the
miner. It was subsequently upgraded to the tested ARM image in validation-only
mode; follow its README for replay/convergence results. Wolf359 was separately
started on K3s with mining disabled. All three deployments preserve current v3.

Craig supplied the independent cloud Explorer check at 03:08:55 UTC: height
71,804, 914 blocks, six transactions, 16.2 shares/minute, and `check=ok` for all
seven accounts at height 71,801 (check time 03:08:46). This is a recent matching
ledger check, three shares behind that stats snapshot. Science paid 31,675 plus
315 escrow equals the 914-block science allocation. The Explorer's `known`
count and canonical height had both increased by 444 from the paused checkpoint.

The overnight capture through 11:20 UTC contained 7,699 consecutive new records
through height 79,059, with the original history unchanged and zero new siblings.
All 1,009 captured status samples reported zero orphans; no local rejections or
fatal errors were logged. Nixos had no restart/OOM, temperature was 52–53 C,
and RSS was approximately 59 MiB. The repaired mini kept pace as a validator.

## Requested 75% duty cap, 2026-10-02

Craig requested 75% mining duty after the overnight run. The prior 25% Compose
file is saved as `incident-20261002-restart/compose.duty25.yml`. The miner stopped
cleanly (exit 0, no OOM) and restarted at 11:49:17 UTC, with the same image,
wallet and data volume, one CPU and 1 GiB memory. The setting is read at startup;
changing it requires a restart and full validated chain replay.

All 226,877 saved records loaded to height 79,480 at 12:21:28 UTC: replay took
1,931 seconds. Mining then resumed at the requested 75%. At 12:38:59 it reported
height 79,819, two peers, zero orphans, 58 C and no restarts/OOM or local errors.
Docker measured approximately one CPU and 59 MiB RAM. The mini's later sample
was at 79,822, consistent with continued propagation; these separate samples
are not an exact-tip independent ledger check. Thermal target remains 82 C,
cap 88 C, with battery and sensor safety intact.

To return to the verified 25% configuration on nixos, retaining all data:

```sh
cd /home/cd/constella-homelab
docker compose -p constella-gate-nixos -f compose.yml stop -t 60 node7
cp incident-20261002-restart/compose.duty25.yml compose.yml
docker compose -p constella-gate-nixos -f compose.yml up -d --no-deps --no-build --pull never node7
```

To disable mining by restoring the previous validation-only configuration on nixos:

```sh
cd /home/cd/constella-homelab
docker compose -p constella-gate-nixos -f compose.yml stop -t 60 node7
cp incident-20261002-restart/compose.paused.yml compose.yml
docker compose -p constella-gate-nixos -f compose.yml up -d --no-deps --no-build --pull never node7
docker compose -p constella-gate-nixos -f compose.yml logs --tail 30 node7
```

Expected after replay: `mode: validation-only`, the retained chain tip and
`duty=0%`. This retains all data, including shares accepted after the upgrade.
Never restore the historical chain copy over the live volume for this rollback.

## Requested 100% duty cap, 2026-10-02

Craig subsequently requested 100%. The 75% configuration is saved remotely as
`duty100-20261002/compose.duty75.yml`. The old container exited 0 without OOM;
the replacement started at 13:25:31 UTC with the same image, wallet and volume.
It validated 227,937 records to height 80,540 in 1,939 seconds, completing at
13:57:50. At 14:06:54 it reported height 80,752, two peers, zero orphans,
100% duty and 57 C, with no restarts/OOM. The one-CPU quota, two workers,
1 GiB limit and real thermal/battery protections remain in force.

Before enabling the Mini's second miner, both nodes returned identical balances
and nonces for the Mini and Nixos addresses at height 80,650. After both miners
were active, the independent Explorer reported `check=ok`, all seven accounts
at height 80,761 (14:07:46 UTC), with its chain at 80,762 nine seconds later.
Normal competing shares can occur with two miners; the earlier zero-sibling
observations apply to the single-miner window only.

To return to 75% while retaining the live wallet and chain:

```sh
cd /home/cd/constella-homelab
docker compose -p constella-gate-nixos -f compose.yml stop -t 60 node7
cp duty100-20261002/compose.duty75.yml compose.yml
docker compose -p constella-gate-nixos -f compose.yml up -d --no-deps --no-build --pull never node7
```

## Wallet and operation

Public testnet payout address (a new wallet unique to this volume):

`78854e90cbea307ec457e5d1713bc3e328f75d16a8b4589859048e01241e9f7b`

The spending key and separate P2P identity remain in the protected data volume.
No private keys were printed or copied into this repository. This is testnet;
custody/restore must be established before relying on any wallet for value.

On nixos:

```sh
cd /home/cd/constella-homelab
docker compose -p constella-gate-nixos -f compose.yml ps
docker compose -p constella-gate-nixos -f compose.yml logs --tail 30 node7
docker stats --no-stream constella-gate-nixos-node7-1
# Stop this node while retaining its wallet and chain:
docker compose -p constella-gate-nixos -f compose.yml stop -t 60 node7
# Resume the same node and data:
docker compose -p constella-gate-nixos -f compose.yml up -d --no-deps --no-build --pull never node7
```

Never remove the data volume or recreate it to troubleshoot a sync problem.
`unless-stopped` resumes a running node after Docker/host restart, but deliberately
stopped containers remain stopped. A host reboot has not yet been tested.
