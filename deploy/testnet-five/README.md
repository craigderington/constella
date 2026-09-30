# Five-node release-gate testnet

This topology runs nodes 1–3 and the explorer locally, with nodes 4–5 on
`asus-tuf-a16`. It uses isolated Compose projects and volumes:

- local project: `constella-gate-local`
- ASUS project: `constella-gate-asus`
- node ports: local `17043`–`17045`, ASUS `17043`–`17044`
- explorer: `http://127.0.0.1:13071`
- Postgres: `127.0.0.1:15439`

The initial topology gives each node one bootstrap edge. Nodes must learn the
other advertised endpoints through `GETADDR`/`ADDR`; a pre-wired all-node peer
list is deliberately not used. Both hosts are on one RFC1918 LAN, so these
files explicitly enable `CONSTELLA_PRIVATE_NET=1`. Never enable that setting
for a public node: it relaxes public netgroup diversity for private endpoints
so multiple lab nodes published on one host can be discovered.

## Acceptance sequence

The explorer/database have a separate `explorer-ui` bridge. Only the explorer
also joins `gate` to follow node1; miners never join the UI bridge. Explorer
startup does not depend on starting node1. During a partition, the explorer
can remain available on localhost while the mining bridge stays internal.
Start only its services (add the partition override when that test is active):

```sh
LOCAL_HOST=192.168.1.212 REMOTE_HOST=192.168.1.174 docker compose \
  -p constella-gate-local -f deploy/testnet-five/compose.local.yml \
  -f deploy/testnet-five/compose.local.partition.yml up -d --no-deps postgres explorer
```

This leaves mining processes and volumes untouched. Explorer data follows
the local fork during a partition and does not establish five-node convergence.

1. Build the node and explorer images from the exact reviewed tree. Record the
   node-binary hash, transfer that image to the ASUS, and do not rebuild
   different source there.
2. Start both groups with `LOCAL_HOST` and `REMOTE_HOST` set to the two LAN
   addresses. Do not wait for a seed connection before starting its target:
   the bootstrap edges form a cycle across the hosts. `GETADDR` runs once per
   connection; reconnect peers if necessary to propagate endpoints learned
   after the initial handshake, then inspect the saved tables.
3. Watch each node's `p2p: discovery learned=...` lines. Every node must know
   the other four endpoints and all five nodes must converge on the same
   tip. Check endpoint identities, not just counts. A node can hold five entries if a peer gossips its own advertised
   endpoint back; outbound selection still refuses to self-dial.
4. Let the chain cross heights 256 and 512. Submit a transaction and require a
   science payout; the explorer must continue reporting a matching ledger.
5. Cleanly stop both node groups so `peers.dat` is saved. Recreate them with
   the matching `compose.*.seedless.yml` override. With `CONSTELLA_PEERS`
   empty, all nodes must reconnect from their learned tables and converge on
   the same prior tip.

Do not use `docker compose down -v`; the volumes are evidence for the later
crash/recovery and reorg gates.

See [GATE2.md](GATE2.md) for the forced partition/reorganization gate.
See [GATE3.md](GATE3.md) for crash/persistence recovery and the ASUS school-trip
audit, and [BUG041.md](BUG041.md) for the zero-duty worker fix.

For the currently healed testnet, apply the matching `compose.*.live.yml`
**after** the partition override. It retains the outbound bridge across node
replacement and selects the tested pause-fix image. Target mining services
explicitly so explorer/database lifecycle stays independent. LAN ports still
use host socat relays. ASUS rollout relay units end in `-pause` and remain
transient; the local host uses the persistent services described below.

## Local reboot recovery

The 2026-09-30 reboot exposed two independent startup issues: temporary relay
services and INPUT rules disappeared, while nodes and the explorer spent
several minutes revalidating their saved share history. The explorer currently
opens HTTP only after its complete database replay; a running container can
therefore reset HTTP connections during startup. Avoid restarting it repeatedly,
which starts validation over again. Nodes likewise open P2P after chain replay.

Local relay source is `relay.sh`, with the user unit
`systemd/constella-gate-relay@.service`. Installed copies live at
`~/.local/libexec/constella-gate-relay` and
`~/.config/systemd/user/constella-gate-relay@.service`. Instances for 17043,
17044 and 17045 are enabled under `default.target`; user lingering is enabled
on this host, so they start at boot without an interactive login. Listeners
retry if the LAN address is not ready. Each incoming connection resolves the
current node's `constella-gate-liveheal` address through Docker, avoiding stale
container IPs after a restart or replacement. The mapping remains node1–3 in
port order, bound only to `192.168.1.212`.

Persistent UFW rules permit TCP 17043–17045 to that local address only from
ASUS (`192.168.1.174`, comment `constella-gate-asus`) and mini
(`192.168.1.123`, comment `constella-gate-mini`). Explorer remains localhost-only
at `http://127.0.0.1:13071`. This repair preserves all containers and volumes.

```sh
systemctl --user status 'constella-gate-relay@*'
docker logs --tail 20 constella-gate-local-node1-1
docker logs --tail 20 constella-gate-local-explorer-1
curl -fsS http://127.0.0.1:13071/healthz
```

When deliberately repartitioning, stop these relay instances and account for
the liveheal network and persistent firewall rules as well as Compose overrides.
