# Docker hosts after K3s retirement

Craig confirmed the Wolf359 backup and K3s uninstall on all three machines
on 2026-10-10, then authorized parallel Docker Engine/Compose installation.
All three rebooted successfully and run Debian 13 amd64:

| Host | LAN address | Installation access |
| --- | --- | --- |
| Wolf359 | 192.168.1.205 | Craig runs staged installer with password sudo |
| Gliese518 | 192.168.1.204 | Agent via existing passwordless sudo |
| Andromeda | 192.168.1.191 | Agent via existing passwordless sudo |

Installed and verified on all three: Engine/CLI 29.9.0, Compose 5.6.0,
Buildx 0.38.0; Docker and containerd active and enabled at boot. Gliese518 and
Andromeda additionally passed client/server queries and the bounded hello-world
test, with zero containers remaining and cgroup v2. Their containerd is 2.4.1.
Craig ran Wolf359's installer; remote unprivileged checks confirmed versions
and service state, but its container smoke-test output was not independently
observed. All three expose only SSH in the final TCP listener check.

`install.sh` follows the [official Docker Debian repository instructions](https://docs.docker.com/engine/install/debian/).
It checks host/OS and K3s removal, refuses conflicting installed packages,
installs the signed stable repository, Engine, CLI, containerd, Buildx and
Compose plugin, enables the services at boot, and runs a disposable bounded
`hello-world` container. It does not start mining or create mining volumes.
After installation, Craig requested Docker group access. The `cd` account is
now in `docker` on all three hosts: Craig changed Wolf359; the agent completed
Gliese518 and Andromeda. Fresh SSH sessions verified `docker info`, `docker ps`
and `docker compose version` without sudo on every host, with zero containers.
Reconnect existing sessions to acquire the new group; multiplexed SSH sessions
may retain old groups. Sudo policy is unchanged. No TCP Docker API is configured.

The identical script is staged on every host:

```sh
sudo sh /home/cd/constella-docker-setup/install.sh
```

Verify:

```sh
sudo systemctl is-active docker containerd
sudo systemctl is-enabled docker containerd
docker version
docker compose version
docker buildx version
docker ps -a
```

Expected: both services active/enabled, client/server versions available,
Compose and Buildx versions printed, and no persistent containers from setup.
Package installation does not upgrade the entire OS. Apply future package
updates deliberately using the host's normal maintenance procedure.

To disable this new runtime before any workloads are deployed:

```sh
sudo systemctl disable --now docker.service docker.socket containerd.service
```

This preserves installed packages and all Docker data. K3s reconstruction is
separate; retain the private Wolf359 retirement backup and old image bundles.
Mainnet storage, final mining profiles and release selection remain subsequent
work. Current testnet deployments are recorded below.

## Testnet nodes commissioned October 10

Craig requested Wolf359 mining and Andromeda validation. Both use the qualified
v5 AMD64 node image `sha256:1d9b37e4d56bed3e46958bdf4fca1b8943ebbf2ed74f1c9d54024e0b414ae6bb`,
chain `2094b0868a27b032`, with new Docker volumes and independent identities.
The retired Wolf359 K3s archives remain intact; neither archive was restored
into these new volumes. Configuration lives at `/home/cd/constella-public-v5`
on each host. Image archives are retained there for recovery.

| Host | Role | Limits | P2P listener | Data volume |
| --- | --- | --- | --- | --- |
| Andromeda | Validation only (`CONSTELLA_MINE=0`), no thermal helper/workers | 1 CPU, 1 GiB, 32 PIDs | 192.168.1.191:18473 | constella-public-v5-andromeda_data |
| Wolf359 | Two workers, 100% duty, real coretemp sensor, cap 88°C / target 82°C | 1 CPU, 1 GiB, 64 PIDs | 192.168.1.205:18473 | constella-public-v5-wolf359_data |

Each node uses a read-only root, dropped capabilities, no-new-privileges,
32 MiB temporary filesystem, three 10 MiB log files and a separate P2P bridge.
Wolf359's helper is separately capped at 0.05 CPU, 16 MiB and 16 PIDs, uses the
qualified image `sha256:791477a4039b24b07fbb1cf43ac8520b0fd54b9dbe10c5a55087dc357333222c`,
and publishes real CPU readings into the host's `thermal` directory. An absent
`thermal/mining_cpu_millidegrees` symlink holds mining at zero while syncing.
The helper has read/write thermal storage; the miner mounts it read-only.
Volume disk consumption is not quota-limited; monitor host disk space.

Both completed peer synchronization from genesis. At height 108524, five queried
public account balances/nonces/next-nonces agreed on NixOS, Wolf359 and Andromeda.
Wolf359 was released at 22:06:35 UTC with a fresh 36°C CPU sample and healthy
public ledger check. Its shares at 108531, 108552 and 108562 were independently
observed on Andromeda. At 22:09 UTC Wolf359 was mining at 25% and 37°C; both
new nodes had five peers, zero orphans, zero restarts and no OOM. Andromeda
remained validation-only. Wolf359's public mining address is
`0116f85b2f0202d0f36e2035d5f3db71dcf78d9ec4acba79b9a198c9219e62c0`.
The existing observation timer now includes both nodes, with validator-aware
checks; its original schedule is unchanged. Startup catch-up/held-sensor warnings
are retained as commissioning evidence, not erased.

Final check at 22:49 UTC: no monitor findings across all five homelab nodes.
Explorer height 109129, ledger `ok` for 5/5 accounts at 109128, four observed
miners. Wolf359 remained at 25% duty and 39°C; Andromeda remained validation-only.
Both had five peers and zero orphans. All nine node/helper containers had zero
automatic restarts and no OOM. Evidence is retained in the ignored
`constella-data-archive/operations/docker-nodes-20261010/postflight/` directory.

On October 11 Craig requested 75% mining duty on Wolf359. Only `MINING_DUTY`
changed from 25 to 75; the node stopped cleanly and replayed with mining held.
All five public account balances/nonces matched NixOS at height 124029.
Mining was released at 15:33:18 UTC after the public checkpoint and a fresh
49°C sensor sample passed verification. At 15:36 UTC it reported 75% duty,
50°C, five peers, zero orphans and no automatic restarts/OOM. New Wolf shares
were independently accepted by Andromeda; the public ledger check passed 5/5.
Images, wallet, data volume, helper and resource/thermal limits are unchanged.
Evidence is in `constella-data-archive/operations/wolf-duty75-20261011/`.

On October 12 Craig requested 100% duty on Wolf359 and suspension of ASUS mining
for laptop travel. ASUS's node and both temperature helpers were stopped; data
and wallets are retained. Its mining sensor alias is absent, and the stopped
containers stay stopped across reboot. ASUS is excluded from active monitoring.

Wolf359 changed only `MINING_DUTY=75` to `100`, preserving the same image, data,
wallet, helper and all resource/thermal limits. Its node stopped cleanly and
replayed held. Before release, NixOS, Mini, Wolf359 and Andromeda agreed on the
full tip and all five configured account states at height 145069, matching the
Explorer's full tip. Release at 15:46:09 UTC passed with a fresh 45°C real sample.
At 15:50 UTC Wolf359 reported 100% duty, 47°C, four peers and zero unresolved
orphans, with no restarts/OOM. Andromeda accepted 15 of its post-release shares.
The final four-node checkpoint passed again at height 145163. The new
recurring [checkpoint monitor](../monitoring/README.md) retains its exact audit
scope and limitations. Operation evidence is in
`constella-data-archive/operations/wolf-duty100-asus-suspend-20261012/`.

The prior 75% configuration is retained privately on Wolf359 at
`/home/cd/constella-public-v5/.env.before-duty100-20261012T152542Z`.
The earlier 25% backup `.env.before-duty75-20261011T151748Z` is also retained.
To roll back the current 100% duty change on Wolf359, hold mining, cleanly stop only the
node, restore the prior configuration, and restart held:

```sh
cd /home/cd/constella-public-v5
docker compose exec -T temperature rm -f /thermal/mining_cpu_millidegrees
docker compose stop -t 60 node
test "$(docker inspect --format '{{.State.ExitCode}}' constella-public-v5-wolf359-node-1)" = 0 &&
  cp -p .env.before-duty100-20261012T152542Z .env &&
  docker compose up -d --no-deps --no-build --pull never node
```

Verify replay completion, peer synchronization and account agreement before
using the sensor-gated release below. Expected afterward: configured duty 75%,
the same wallet/volume, zero unresolved orphans and safe real CPU temperature.

The retired Wolf359 backup's checksums were verified. Its public history reached
only header height 49020, behind the new nodes when inspected, so it was not
used to bootstrap them. The extracted public file stays under Wolf359's
`constella-public-v5/bootstrap`; all archived keys and backup files remain intact.

Before maintenance on Wolf359, hold mining, then stop only these new services:

```sh
cd /home/cd/constella-public-v5
rm -f thermal/mining_cpu_millidegrees
docker compose stop -t 60 node temperature
```

To resume Wolf359, start held, verify sync and account agreement, then release
only with a fresh real sample below the configured cap:

```sh
cd /home/cd/constella-public-v5
rm -f thermal/mining_cpu_millidegrees
docker compose up -d --no-deps --no-build --pull never temperature node
docker compose logs --tail 30 node
```

Release is a separate operator step after synchronization; no automatic
synchronization gate is provided by this profile. A previously released alias
persists across automatic restarts. After verifying ledger/account agreement,
release Wolf359 with a fresh real sample:

```sh
docker compose exec -T temperature sh -eu -c '
  sample=/thermal/cpu_millidegrees
  age=$(( $(date +%s) - $(stat -c %Y "$sample") ))
  value=$(cat "$sample")
  test "$age" -ge 0 && test "$age" -le 3
  test "$value" -ge 1000 && test "$value" -lt 88000
  ln -s cpu_millidegrees /thermal/mining_cpu_millidegrees
'
```

For Andromeda stop/resume only `node`:

```sh
cd /home/cd/constella-public-v5
docker compose stop -t 60 node
# Resume when wanted:
docker compose up -d --no-deps --no-build --pull never node
```

Stopping these services is the deployment rollback; retain all data volumes,
keys, images and configuration. Never use `down -v` or delete volumes.

Gliese518 remains unused. The confirmed future split is ASUS/NixOS continuing
testnet mining; Mini/Wolf359/Gliese518 mining mainnet after qualification and
launch approval. New mainnet volumes/keys and explicit mainnet artifacts are
required; testnet state and funds do not migrate to mainnet.
