# ASUS node4 canary

Craig authorized restarting one miner on the homelab ASUS on 2026-10-02.
Host: cd@192.168.1.174 (asus-tuf-a16). This is not the production Lightsail host.
The other ASUS node, node5, remains stopped. Mini, NixOS, Wolf359 and production
were not restarted for this deployment.

The frozen trial image is constella:dns-payload-amd64-20261002, code at 3189f27:
the five initial hardening fixes plus bounded DNS, payload ownership and the
container PID 1 resolver correction. Its image ID was verified on both hosts as
sha256:c4bb147ff54492be56609c7ba6b4b1e89348934ff9ff567072e06e57bf583959.
Do not rebuild/replace this tag with ongoing ledger or Explorer development.

The 48-hour mining trial began 2026-10-03 15:20:53 UTC (11:20:53 Eastern).
Its earliest completion is 2026-10-05 15:20:53 UTC. The previous process exited
cleanly with status 0. The new process replayed and caught up with mining held;
ASUS and Mini then returned the same tip at height 103140:
0dbea5b51e5221f81344a0e8db387410a21d9dbeeee551c643b5a4147540114d,
balance 11113.16120236, nonce 1, next nonce 1. Mining was released with a fresh
35.75 C CPU sample. The exact checkpoint is saved on ASUS in trial-3189f27.json.
Elapsed time alone is not a pass: compare current tip/accounts with a healthy
peer, inspect rejects/competing shares, temperature, restarts/OOM and logs.
This record is not an unattended monitoring or automatic promotion service.

Compose project: constella-gate-asus. Only node4 and the small temperature
publisher are managed by this profile. The original external volume
constella-gate-asus_node4 contains the retained wallet, identity and v3 chain.
Both key files were mode 0600, owned by UID 0; the old and new binaries returned
the same public payout address. No private keys were copied or displayed.
The previous container configuration, image and Compose files remain on ASUS.

Operational files are under
/home/cd/constella-release-gate/asus-audit-20261002. The node uses two workers,
a one-CPU/1 GiB cap, 25% maximum duty, temperature cap 80 C/target 74 C, battery
pause, a read-only root filesystem and no added capabilities. It connects out
to the cloud validator and Mini; it publishes no host port. The temperature
publisher runs as ASUS's cd user (UID/GID 1000), without networking or added
capabilities, and copies the actual k10temp CPU reading once a second. Missing,
malformed or stale samples cause the node to stop mining.

## Synchronization and mining release

The sensor alias thermal/mining_cpu_millidegrees was deliberately absent during
replay and synchronization, keeping duty, searches and finds at zero. The real
sensor publisher ran independently throughout. This is an operator-controlled
catch-up hold, not an automatic synchronization gate on future restarts.

Authenticated read-only queries to ASUS and Mini agreed at height 84854:

- Tip: 01bea439c123ccff771011a873534046c2191b570476df42259bb94cad464efa
- ASUS payout: 3ec619ac279656341b7782b6e445238104e593e87c2fc4511e3bfe948102da49
- Balance: 7003.24766302, nonce 1, next nonce 1

For the preceding hardening build, mining was released at 18:39:56 UTC using the live CPU
sample (33.125 C, 0.56 seconds old). At 19:54:04 UTC ASUS reported h=86022,
two peers, zero orphans, 25% duty, 53 C, 467 finds and about 3.4 million
candidates/second. Its own shares appeared on the advancing chain. No container
restart or OOM was observed; node5 remained exited.

## Inspect or pause

Run on ASUS, never on production:

```sh
cd /home/cd/constella-release-gate/asus-audit-20261002
docker compose ps
docker compose logs --tail 30 node4
docker compose logs --tail 10 temperature
```

For a planned restart or loss of synchronization, close the mining hold first:

```sh
cd /home/cd/constella-release-gate/asus-audit-20261002
rm -f thermal/mining_cpu_millidegrees
```

Within a few seconds, status should show duty=0%, sensor unavailable, and no new
local search work. Peer validation and relay continue. After comparing chain
and ledger state with a healthy peer, release only against the actual sample:

```sh
cd /home/cd/constella-release-gate/asus-audit-20261002
ln -s cpu_millidegrees thermal/mining_cpu_millidegrees
```

If the node itself needs to stop, retain its data:

```sh
cd /home/cd/constella-release-gate/asus-audit-20261002
docker compose stop -t 60 node4
```

A validation-only fallback is available in compose.validation.yml:

```sh
cd /home/cd/constella-release-gate/asus-audit-20261002
docker compose -f compose.yml -f compose.validation.yml up -d --no-deps --no-build --pull never node4
```

Verify the startup message says validation-only, its wallet is not loaded for
mining, and height advances after replay. This keeps the hardened image and the
same volume. If the trial code needs rollback, compose.before-dns.yml on ASUS
retains the previously tested audit-hardening image. Hold mining first, then:

```sh
cd /home/cd/constella-release-gate/asus-audit-20261002
docker compose -f compose.before-dns.yml up -d --no-deps --no-build --pull never node4
```

Keep the hold until replay/convergence and account checks pass again. Any image
change or restart must be recorded when assessing the trial. Do not fall back
to mining with the old pause-fix image: it predates
the competing-share repair. Never remove volumes or use --remove-orphans here;
the retained node5 container is deliberately outside this profile.
