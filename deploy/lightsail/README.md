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
Classline receive no deployment or configuration changes. Craig provisioned
a Debian clone of Status Pulse at static IPv4 **3.150.62.26**, removed the
cloned Status Pulse containers and repository, and confirmed the node and
explorer image builds passed there. Runtime capacity still needs verification.
Use x86_64 Linux for these images; see [host and HTTPS setup](HOST-SETUP.md).

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

Craig explicitly chose to build from the downloaded repository on the new host.
Default builds preserve v3. Keep the required tracked tests even though new
test/evidence files are ignored. The node's Makefile requires the validator
regression added in `b874603`; an earlier checkout fails at that missing file.
Record the checkout commit and image IDs. Both embedded build/test suites must
pass. No agent runs these commands on the host or pushes Git.

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
# Craig confirmed these two application builds passed. Record the results.
git rev-parse HEAD
sudo docker image inspect --format '{{.Id}}' constella:lightsail-testnet-20261001
sudo docker image inspect --format '{{.Id}}' constella-explorer:lightsail-testnet-20261001
sudo docker pull postgres:16-alpine
sudo docker image inspect --format '{{.Id}}' postgres:16-alpine

sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml config --quiet
# Start services by name so explorer/database operations cannot restart mining peers.
sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml up -d --no-deps postgres node
sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml up -d explorer

sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml ps
sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml logs --tail 60 node explorer postgres
curl --fail --silent --show-error http://127.0.0.1:3071/api/stats
curl --silent --show-error --include http://127.0.0.1:3071/healthz
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
During a deliberate mining pause, `/healthz` can return 503 `indexer stale`
because it requires a chain-state update within five minutes. Confirm dashboard
availability with `/api/stats`, and inspect peer/tip and fresh account-check
metadata separately; database errors remain faults.

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

At 2026-10-02 02:14 UTC, Craig installed the prepared Apache vhosts and obtained
the certificate for `explorer.catasterism.xyz` (expires 2026-12-31). Both Apache
config checks passed, and a trusted HTTPS request returned the expected height
71,360, full tip `3e1424d8f0e31ec8bc230ba48a3d921c1e9349adc94e01c9aab0c33886b3fc8c`
and a fresh six-account `check=ok`. The renewal timer and deploy hook were
installed by the same operator-run script. Craig subsequently confirmed the
dashboard loads and the hostname-scoped renewal dry-run succeeded. Live mining
remains paused for the
competing-share incident; see [mini candidate verification](../testnet-mini/README.md).

Craig reported successful startup and historical catch-up on 2026-10-01.
At height 14,315 the node had three peers, validation-only mode, zero found
shares and a growing orphan queue from newer live shares arriving before
their parents. Catch-up must resolve that queue; it is not final healthy-state
evidence. The displayed ledger totals are cumulative history, not this host's
new earnings. At 190 blocks, reported science-paid 6,335.00000059 plus escrow
314.99999941 equals the configured 190 × 35 coin science allocation.

An explorer connected before its upstream downloaded history remained at
genesis. Code inspection found no periodic idle GETCHAIN request, and the node
does not broadcast old shares during replay. Craig restarted only the explorer
to trigger a fresh request. Verify subsequent progress and final ledger checks;
a regression and bounded resync fix remain work items. Do not reset databases
or chain volumes to work around this startup-following behavior.

- New-host OS/IP/listener inventory, workload baselines and resource-limit acceptance.
- Sustained bootstrap/homelab connectivity and ledger agreement after mining resumes.
- Remaining firewall/exposure checks; browser, HTTPS API and renewal dry-run verified above.
- Blank-host backup/restore, alerts, host reboot and a sustained dedicated-host run.

These prerequisites are not automatically satisfied by the calendar date.
