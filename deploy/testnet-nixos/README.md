# Homelab node7 on nixos

Craig authorized starting this node on 2026-10-01. It runs independently of
the cloud stack and mini; no production commands are executed by agents.
Host `nixos` is x86_64, four logical CPUs, 31 GiB RAM and 843 GiB free root
disk at provisioning. Clock synchronization was confirmed. Existing unrelated
containers were left unchanged.

Deployment: `/home/cd/constella-homelab/compose.yml` on nixos.
Project: `constella-gate-nixos`; container: `constella-gate-nixos-node7-1`;
data volume: `constella-gate-nixos_node7`. No host ports are published.
Explicit outbound peers are Lightsail `3.150.62.26:7043` and mini
`192.168.1.123:17046`; other private-address discovery remains disabled.
No host firewall or NixOS system configuration was changed.

The previously tested amd64 node image was transferred directly from local
Docker and loaded on nixos, without a registry push or rebuild. Both sides
reported image ID
`sha256:cca575a931ea3f1d46376d6d619ef466705f8ca6a3d8f7a69194d5ca43b9db51`
under `constella:lightsail-testnet-20261001`. This image can mine; the cloud
deployment disables mining through its explicit environment configuration.

Node7 uses two workers, 50% duty, a one-CPU container quota, 1 GiB memory cap
and the real `x86_pkg_temp` sensor. Target is 82 C, configured cap 88 C,
with battery/sensor safety retained. On initial join it reported 48 C, two
authenticated peers, zero orphans and height 334. Catch-up is still in progress;
that observation is not a completed ledger/convergence or sustained-capacity test.

Mining was then disabled with `CONSTELLA_MINE=0` for initial synchronization:
the first startup produced a few low-height side-branch shares while downloading
history. The existing wallet volume is retained. Enable mining only after node7
matches the retained chain and its ledger has been checked; the two-worker
thermal settings above apply when mining is enabled. Validation-only mode has
no mining or thermal-sampler threads. This pending enablement is not automatic.

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
