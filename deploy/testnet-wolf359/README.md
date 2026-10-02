# Homelab node8 on Wolf359

Craig authorized this node on 2026-10-02 UTC while nixos replayed its saved
history. Wolf359 is a Debian 13 amd64 host at `192.168.1.205`, with four logical
CPUs, 15 GiB RAM, 200 GiB free root disk and a real `x86_pkg_temp` sensor.
Clock synchronization was confirmed. It already runs K3s; Docker is not needed.

Craig confirmed all three K3s members Ready (`wolf359`, `gliese518`,
`andromeda`), version `v1.36.4+k3s1`, containerd `2.3.4-k3s1.36`, and the
`local-path` storage class with `WaitForFirstConsumer` binding. The SSH user
cannot read the root-owned admin kubeconfig or use passwordless sudo. Craig
runs the prepared script; no permission broadening is required.

## Deployment

Bundle directory on Wolf359: `/home/cd/constella-wolf359`.

The bundle contains `namespace.yml`, `node.yml`, `start.sh`, and a single-platform
Docker archive of the tested image `constella:burst-fix-amd64-20261002`.
The image archive SHA-256 is
`eca409563dd56eea36bdc9a65caa9b2ee3470773103267ed1630650cfff4c934`.
`start.sh` verifies that checksum, host/node identity, readiness and storage
provider, imports into K3s containerd, and validates the manifests with the API
server before applying the workload. It creates only the Constella namespace
and named workload resources. It does not install Docker or alter K3s services.

On Wolf359, review and start:

```sh
cd /home/cd/constella-wolf359
cat start.sh namespace.yml node.yml
sudo sh ./start.sh
sudo k3s kubectl -n constella-testnet get pods,pvc -o wide
sudo k3s kubectl -n constella-testnet logs -f node8-0 --tail=30
```

Expected: pod `node8-0` assigned to `wolf359`, PVC `data-node8-0` Bound,
`mode: validation-only`, chain `a8f4562e57e74f9d`, authenticated peers and an
advancing height while synchronizing. Genesis/height zero is expected initially.
The pod's readiness means its P2P listener is open, not that it is synchronized.
There is no liveness deadline that would kill a long validated startup replay.
Readiness opens and closes a TCP connection; this may produce a periodic generic
handshake warning. Compare peer counts and progress separately.

One StatefulSet replica is pinned to Wolf359. It has a 250m CPU/256 MiB memory
request and a one-CPU/1 GiB limit, read-only root filesystem, dropped capabilities,
no service account token, and 32 MiB memory-backed `/tmp`. The service is
headless and cluster-internal; no host ports, ingress, NodePort or load balancer
are provisioned. It connects outbound to Lightsail and the mini as explicit peers.
Existing K3s applications are not changed.

Mining remains disabled until synchronization and ledger agreement are checked
and the first connected miner on nixos is stable. No wallet is generated in
validation-only mode; the independent P2P identity and chain persist in `/data`.
Enabling mining later must retain this volume and verify the real temperature
sensor. The prepared future miner settings are two workers at 25% duty.

## Storage and pause

The PVC requests 20 GiB from `local-path`. This is node-local storage, not a
replicated backup or an enforced filesystem quota; monitor actual host free
space. PVC retention is explicitly `Retain` on StatefulSet deletion/scaling.
The storage class itself uses reclaim policy `Delete`: deleting the PVC or its
namespace can delete its data. Never use either as a troubleshooting reset.

Pause this node while retaining its identity and history:

```sh
sudo k3s kubectl -n constella-testnet scale statefulset/node8 --replicas=0
```

Resume the same node and volume:

```sh
sudo k3s kubectl -n constella-testnet scale statefulset/node8 --replicas=1
```

Manifests were parsed locally and server-side dry runs passed on Craig's K3s
cluster. The first host-address guard rejected the dual-stack InternalIP list;
it was corrected to require the expected IPv4 among the returned addresses.
The guard stopped before importing or creating any resources on that attempt.

Craig ran the corrected script successfully. K3s imported the tested amd64
manifest (`sha256:37fae61a5452cf81eb486a30e5586ea1f4954f7bb1eae833cd82fa0ff86f03b0`).
At 90 seconds the pod was Running/Ready on Wolf359 at `10.42.0.77`, with zero
restarts and PVC `data-node8-0` Bound to
`pvc-efebb98a-9c14-4328-88dd-22f96222380c`. Logs confirmed startup at 02:57:37 UTC,
validation-only mode, v3 testnet identity `be0fdf387d87cf06`, and two peers.
Height advanced from 342 to 664 to 1,026 by 02:59:07. The 79 queued orphans in
that last sample are catch-up observations, not evidence of completed sync;
verify they drain and compare the live tip/ledger before enabling mining.

This is homelab testnet only. No production commands, registry pushes or Git
pushes are part of this deployment.

References: [K3s cluster access](https://docs.k3s.io/cluster-access),
[K3s local storage](https://docs.k3s.io/add-ons/storage),
[Kubernetes StatefulSets](https://kubernetes.io/docs/concepts/workloads/controllers/statefulset/).
