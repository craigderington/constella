# Status Pulse — operator-run testnet rollout

Target window: **Friday, 2026-10-02, 08:00 America/New_York**. Craig performs
all production commands. Agents do not SSH into Status Pulse, deploy, change
its firewall/DNS/proxy, or push Git. This is the existing **v3 testnet**
(`chain=a8f4562e57e74f9d`), not the isolated v4 candidate or valuable-coin mainnet.
The one-month-or-longer testnet campaign and mainnet audit gates still apply.

The cloud node explicitly uses `CONSTELLA_MINE=0`: validation, transaction
admission, gossip and serving the explorer continue, while no mining workers,
thermal sampler or spending wallet are loaded/created. The separate `node.key`
remains its P2P identity. A cloud VM without sensors does not need fabricated
readings. Homelab miners supply new work; a validator alone does not advance
an idle chain.

## Before the deployment window

Run `bash deploy/lightsail/preflight.sh` on Status Pulse and review its output.
This is read-only and intentionally prints no container environment variables.
Craig's inventory: x86_64, 3.8 GiB RAM with 2.4 GiB available, 2 GiB unused
swap, and 66 GiB free on a 79 GiB root filesystem. Existing workloads are
Status Pulse (web, jobs, Postgres) and Classline (app, public, Postgres).
Their loopback ports 3000, 8041 and 8073 must remain untouched. The host runs Debian 13 (trixie) with two CPUs and Apache; Nginx is inactive.
A complete listener inventory and baseline application measurements are pending.

Provisional caps are 384 MiB for the node, 640 MiB for the explorer and 384 MiB
for Postgres: 1.375 GiB total, leaving about 1 GiB of the reported available
RAM. These are limits, not measured capacity guarantees. Provisional CPU caps are 0.50 core each for the node and explorer, and 0.25
for Postgres, totaling 1.25 of the two cores. Swap is emergency headroom, not a basis
for sizing. Confirm no sustained swapping or OOM/restart loop under catch-up
and normal indexing load before accepting these limits.

The local overnight run was healthy, but Postgres checkpoint logs show about
25 GiB of WAL distance over 14 hours. That is a write-load observation, not
retained disk size. Full-history ledger/SQL work remains an audit gate. Set
memory, I/O, disk and sync-lag acceptance limits around the existing Status
Pulse workload before approving this colocated testnet deployment.

Prepare a reviewed commit and an image bundle built for the target architecture.
Default builds preserve v3. Record image IDs and bundle SHA-256 locally. Craig
pushes the commits and transfers the bundle; do not build unreviewed source on
production. Keep the previous Status Pulse configuration and image references.
No existing Status Pulse service is part of this Compose project.

Copy `.env.example` to `production.env`, set mode 0600, choose limits and public
P2P endpoint, and generate a dedicated random hex database password. Do not
paste `docker compose config` output publicly: it contains that password. Use
`config --quiet` for validation. Both the node and database use new named
volumes. Never mount a v4 candidate volume or import its balances.

Bootstrap needs a verified route: either a reachable peer in `NODE_PEERS`, or
a homelab node explicitly connecting outbound to the cloud P2P endpoint. A
private `192.168.*` address is not reachable from Lightsail without a configured
VPN. Empty `NODE_PEERS` selects the existing DNS fallback; its existence alone
is not evidence of successful bootstrap. Do not approve the rollout until the
cloud catches up to a known live testnet peer and the explorer checks the ledger.
The mini node was left running when the other five testnet nodes were stopped.

Only publish the chosen P2P port and the existing HTTPS proxy. Postgres has
**no host port**; the explorer binds host loopback. Confirm both Lightsail and
Docker/host firewall behavior, including IPv6. Scope testnet P2P exposure to
known peers during the controlled campaign where practical.

## Commands for Craig after prerequisites are verified

From the reviewed repository checkout on Status Pulse:

```sh
sha256sum --check /path/to/constella-release-images.tar.sha256
docker load --input /path/to/constella-release-images.tar
# Compare all loaded image IDs against the release manifest before continuing.
docker image inspect --format '{{.Id}}' constella:lightsail-testnet-20261001
docker image inspect --format '{{.Id}}' constella-explorer:lightsail-testnet-20261001
docker image inspect --format '{{.Id}}' postgres:16-alpine

docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml config --quiet
# Start services by name so explorer/database operations cannot restart mining peers.
docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml up -d --no-deps postgres node
docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml up -d explorer

docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml ps
docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml logs --tail 60 node explorer postgres
curl --fail --silent --show-error http://127.0.0.1:3071/healthz
curl --fail --silent --show-error http://127.0.0.1:3071/api/stats
```

Use the configured HTTP port if inventory requires changing 3071. The node
must log `mode: validation-only`, `threads=0`, `duty<=0%` and the v3 chain ID.
It must not create `wallet.key`. A disconnected/replaying node is not ready:
wait for peer connection, current height/tip and a fresh explorer ledger
`check=ok` covering every account. Check restart/OOM counts and the original
Status Pulse endpoints under load. Healthy startup is not proof of sustained
shared-host capacity.

The public HTTPS vhost must be prepared for the web server actually installed
on Status Pulse. The old Apache example contains its HTTPS proxy only as
commented documentation; do not assume certbot completes application routing.
Keep the explorer loopback-only until the real vhost, TLS renewal and existing
Status Pulse routes have been tested. Do not install an Apache configuration
on an Nginx host or replace the existing default vhost.

## Stop / rollback

If the new workload affects Status Pulse, stop only this project:

```sh
docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml stop -t 60 explorer node postgres
```

Retain all named volumes. Never use `down -v` or a broad Docker/system prune.
Remove/disable only the new explorer vhost if needed, validate the web-server
configuration and have Craig reload it. Preserve logs and database/chain
backups before retrying. Do not point older binaries at newer database schemas
without a tested restore plan. The existing Status Pulse application should
require no rollback because these commands do not modify it.

## Launch gates still pending

- Listener inventory, workload baselines and resource-limit acceptance.
- Reachable bootstrap/homelab route and catch-up/ledger agreement.
- Real HTTPS vhost, DNS/TLS and firewall checks.
- Blank-host backup/restore, alerts, host reboot and a sustained shared-host run.

These prerequisites are not automatically satisfied by the calendar date.
