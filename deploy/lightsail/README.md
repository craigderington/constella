# Dedicated Lightsail — operator-run testnet rollout

Target window: **Friday, 2026-10-02, 08:00 America/New_York**. Craig performs
all production commands. Agents do not SSH into production, deploy, change
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

Craig selected a **dedicated 4 GB RAM / 2-vCPU / 80 GB disk instance** on
2026-10-01. This replaces the earlier Status Pulse proposal; Status Pulse and
Classline receive no deployment or configuration changes. The new public IP,
actual OS image and provisioned capacity still need operator verification.
Use x86_64 Linux for the tested images. Debian 12 or 13 is the intended base;
see [host and HTTPS setup](HOST-SETUP.md). Do not assume the inventory from
Status Pulse describes a newly provisioned host.

Run `sudo bash deploy/lightsail/preflight.sh` on the dedicated instance.
It is read-only and prints no container environment variables. Initial caps
are 384 MiB for the node, 640 MiB for the explorer and 384 MiB for Postgres
(1.375 GiB total). CPU caps are 0.50 core each for node/explorer and 0.25 for
Postgres (1.25 cores total). They preserve headroom for the OS, Apache and
backups; they are conservative starting limits, not capacity guarantees.
Full-history catch-up and sustained operation must establish whether these
caps need adjustment. Do not raise them to conceal a leak or replay bottleneck.

The overnight run was healthy, but Postgres checkpoint logs show about 25 GiB
of WAL distance over 14 hours: approximately 0.5 MiB/s on average, not retained
disk growth. Full-history ledger/SQL scaling remains an audit gate even on a
dedicated machine. Monitor CPU burst capacity as well as utilization; Lightsail
is burstable and a short benchmark cannot establish sustained CPU capacity.
See [AWS burst monitoring](https://docs.aws.amazon.com/lightsail/latest/userguide/amazon-lightsail-viewing-instance-burst-capacity.html).

Prepare a reviewed commit and an image bundle built for the target architecture.
Default builds preserve v3. Record image IDs and bundle SHA-256 locally. Craig
pushes the commits and transfers the bundle; do not build unreviewed source on
production. Keep a record of the new host configuration and exact image references.

Copy `.env.example` to `production.env`, set mode 0600, choose limits and public
P2P endpoint, and generate a dedicated random hex database password. Do not
paste `sudo docker compose config` output publicly: it contains that password. Use
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

From the reviewed repository checkout on the dedicated instance:

```sh
sha256sum --check /path/to/constella-release-images.tar.sha256
sudo docker load --input /path/to/constella-release-images.tar
# Compare all loaded image IDs against the release manifest before continuing.
sudo docker image inspect --format '{{.Id}}' constella:lightsail-testnet-20261001
sudo docker image inspect --format '{{.Id}}' constella-explorer:lightsail-testnet-20261001
sudo docker image inspect --format '{{.Id}}' postgres:16-alpine

sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml config --quiet
# Start services by name so explorer/database operations cannot restart mining peers.
sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml up -d --no-deps postgres node
sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml up -d explorer

sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml ps
sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml logs --tail 60 node explorer postgres
curl --fail --silent --show-error http://127.0.0.1:3071/healthz
curl --fail --silent --show-error http://127.0.0.1:3071/api/stats
```

Use the configured HTTP port if inventory requires changing 3071. The node
must log `mode: validation-only`, `threads=0`, `duty<=0%` and the v3 chain ID.
It must not create `wallet.key`. A disconnected/replaying node is not ready:
wait for peer connection, current height/tip and a fresh explorer ledger
`check=ok` covering every account. Check restart/OOM counts, host memory, disk, I/O latency and CPU burst balance
under load. Healthy startup is not proof of sustained capacity.

Follow [HOST-SETUP.md](HOST-SETUP.md) for the actual HTTP/HTTPS vhosts,
certificate issuance and renewal hook. These use `certbot certonly --webroot`;
do not combine them with the older `deploy/apache` automatic-vhost instructions.
Keep the explorer loopback-only. Validate TLS, redirects, certificate renewal,
clock synchronization and remote port exposure before calling it public-ready.

## Stop / rollback

If the new workload is unhealthy, stop only this project:

```sh
sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml stop -t 60 explorer node postgres
```

Retain all named volumes. Never use `down -v` or a broad Docker/system prune.
Remove/disable only the new explorer vhost if needed, validate the web-server
configuration and have Craig reload it. Preserve logs and database/chain
backups before retrying. Do not point older binaries at newer database schemas
without a tested restore plan. Status Pulse and Classline are outside this host and require no rollback.

## Launch gates still pending

- New-host OS/IP/listener inventory, workload baselines and resource-limit acceptance.
- Reachable bootstrap/homelab route and catch-up/ledger agreement.
- Real HTTPS vhost, DNS/TLS and firewall checks.
- Blank-host backup/restore, alerts, host reboot and a sustained dedicated-host run.

These prerequisites are not automatically satisfied by the calendar date.
