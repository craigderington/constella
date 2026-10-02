# Craig-run setup on the new dedicated instance

These commands are for the **new dedicated Constella instance**, never Status
Pulse. Craig runs them. No agent has accessed or provisioned this host.

Choose 4 GB RAM, two vCPUs, 80 GB disk and x86_64 Linux. These instructions
target Debian 12/13; first confirm `/etc/os-release` and `uname -m`. Attach a
Lightsail static IPv4 address and record it. Point `explorer.catasterism.xyz`
at that address. Do not change the apex/marketing DNS. Remove a stale explorer
AAAA record unless this host's IPv6 path is also configured and tested.

In the Lightsail firewall, allow TCP 22 only from Craig's administration IP,
TCP 80/443 for HTTP/TLS, and TCP 7043 from intended testnet peer public IPs
initially. Do not open 3071 or any Postgres port. Inspect IPv6 rules separately.
Docker-published ports need their own exposure check; do not assume a host UFW
rule filters them. Before restricting SSH, preserve a working session and
verify a second login. Do not copy wallet/spending keys to this validator.

## Fresh Debian host

Install security updates and reboot if required before introducing state.
Verify NTP synchronization (`timedatectl show -p NTPSynchronized`). Install
Docker from its official repository on a fresh host without conflicting
distribution Docker/containerd packages. Commands follow the
[Docker Debian instructions](https://docs.docker.com/engine/install/debian/):

```sh
sudo apt update
sudo apt install ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/debian/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc
sudo tee /etc/apt/sources.list.d/docker.sources <<EOF
Types: deb
URIs: https://download.docker.com/linux/debian
Suites: $(. /etc/os-release && echo "$VERSION_CODENAME")
Components: stable
Architectures: $(dpkg --print-architecture)
Signed-By: /etc/apt/keyrings/docker.asc
EOF
sudo apt update
sudo apt install docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo apt install apache2 certbot openssl dnsutils git
sudo systemctl enable --now docker apache2
sudo docker version
sudo docker compose version
```

Craig chose to build the application images from the downloaded, reviewed
checkout on the dedicated host. Retain the tests required by both Dockerfiles;
both builds must pass. Record the image IDs as described in [README.md](README.md).
Use `sudo docker` rather than opening the Docker socket to other users.
Record installed package versions and the instance's static IP in your private
operator notes. Run `sudo bash deploy/lightsail/preflight.sh` before startup.

## HTTP challenge and HTTPS proxy

These files implement the proxy explicitly. Certbot obtains the certificate
without generating another vhost; do not run `certbot --apache` alongside them.
Confirm `explorer.catasterism.xyz` is the intended public hostname before
installation. If changing it, update both vhosts, certificate paths and all
commands together. Confirm no duplicate enabled vhost with `apache2ctl -S`.

Run from the repository root after public DNS resolves to the static IP:

```sh
dig +short A explorer.catasterism.xyz
dig +short AAAA explorer.catasterism.xyz
sudo apache2ctl -S
sudo a2enmod proxy proxy_http headers ssl rewrite
sudo install -d -m 0755 /var/www/constella-acme/.well-known/acme-challenge
sudo install -m 0644 deploy/lightsail/explorer-http.conf /etc/apache2/sites-available/constella-http.conf
sudo a2ensite constella-http
sudo apache2ctl configtest && sudo systemctl reload apache2
printf 'constella-acme-check\n' | sudo tee /var/www/constella-acme/.well-known/acme-challenge/probe
```

From a different internet connection, request
`http://explorer.catasterism.xyz/.well-known/acme-challenge/probe` and expect
HTTP 200 with `constella-acme-check`, without a redirect. Other HTTP paths
redirect to HTTPS, which will not work until the next steps complete.
Then issue the certificate (Certbot prompts for the operator's contact email):

```sh
sudo certbot certonly --webroot -w /var/www/constella-acme --cert-name explorer.catasterism.xyz -d explorer.catasterism.xyz
sudo install -m 0644 deploy/lightsail/explorer-https.conf /etc/apache2/sites-available/constella-https.conf
sudo a2ensite constella-https
sudo apache2ctl configtest && sudo systemctl reload apache2
sudo install -d -m 0755 /etc/letsencrypt/renewal-hooks/deploy
sudo install -m 0755 deploy/lightsail/certbot-reload-apache.sh /etc/letsencrypt/renewal-hooks/deploy/constella-apache
sudo systemctl enable --now certbot.timer
sudo /etc/letsencrypt/renewal-hooks/deploy/constella-apache
sudo certbot renew --cert-name explorer.catasterism.xyz --dry-run
systemctl list-timers certbot.timer
curl --fail --silent --show-error https://explorer.catasterism.xyz/api/stats
curl --silent --show-error --include https://explorer.catasterism.xyz/healthz
```

Expect `Syntax OK`, a successful renewal dry-run, valid hostname/trust-chain
verification without `curl -k`, and the same height/tip/ledger state as the
loopback endpoint. Confirm the actual browser dashboard too.
During a deliberate mining pause, `/healthz` currently returns HTTP 503
`indexer stale` after five minutes without a changed chain state. This alone
does not mean Apache or the dashboard is broken. Use `/api/stats` for the
proxy/TLS availability check, and inspect `peer`, height/tip and fresh ledger
check metadata separately. A database error or failure of `/api/stats` remains
a fault; do not dismiss every 503 as the pause. Do not restart services just
to refresh the chain timestamp. The deploy hook
loads renewed certificates after successful renewal; test its config/reload
path separately because a dry-run need not invoke deploy hooks.
References: [Apache proxy](https://httpd.apache.org/docs/2.4/mod/mod_proxy.html),
[Certbot webroot and renewal](https://eff-certbot.readthedocs.io/en/stable/using.html).

The default proxy upstream is loopback port 3071. If that Compose port changes,
change both `ProxyPass` directives before validating/reloading. Apache logs
use its normal Debian logrotate policy; confirm the Constella log filenames
are included. Keep the existing default vhost until the new hostname passes
verification; do not make an untested vhost the default accidentally.

## Acceptance, recovery and rollback

Monitor free disk/inodes, available RAM, swap activity, container memory and
restart/OOM counts, CPU burst capacity, database size/WAL size, explorer health,
peer count and tip lag. Start with a disk warning at 70% and urgent action at
80%; no OOM/restart loop is acceptable. On a live mining network, investigate
lag above 60 seconds sustained for five minutes. A stopped mining network
needs a different freshness expectation. These are initial operator thresholds,
not established capacity results. Configure delivery to Craig before leaving
the testnet unattended; an agent has not configured external alerts.

Back up the dedicated database and node data to access-controlled off-host
storage. Node data contains `node.key` (P2P identity), even though this node
does not have a spending wallet. Preserve file ownership and mode on restore;
the node drops all capabilities and cannot override another UID's permissions.
Test restoration into fresh disposable volumes before relying on the backup.
Instance snapshots supplement a tested application backup; they do not replace it.
Never make a live filesystem copy of Postgres's data directory as a DB backup.

For a consistent maintenance checkpoint, stop explorer and node, keep Postgres
running for `pg_dump`, and preserve the stopped node volume. Run from repo root:

```sh
umask 077
backup_dir="$HOME/constella-backup-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -m 0700 "$backup_dir"
cloud() { sudo docker compose --env-file deploy/lightsail/production.env -f deploy/lightsail/compose.yml "$@"; }
cloud stop -t 60 explorer node
cloud exec -T postgres pg_dump -U constella -d explorer -Fc > "$backup_dir/explorer.pgcustom"
node_container=$(cloud ps -a -q node)
sudo docker cp "$node_container:/data/." - > "$backup_dir/node-data.tar"
sha256sum "$backup_dir/explorer.pgcustom" "$backup_dir/node-data.tar" > "$backup_dir/SHA256SUMS"
cloud start node explorer
```

Check each command succeeds before proceeding. A failed dump must not be
treated as a valid checkpoint. Record height/tip and schema version with the
backup and verify catch-up after restart. Archive checksums validate bytes,
not restore correctness. The local retained shutdown dump is not a complete
custody backup and is not automatically installed on the new host.

Before the unattended run, Craig performs a deliberate host reboot and verifies
Docker/Apache, all three containers, TLS, peer catch-up and ledger agreement.
`unless-stopped` restarts running services after reboot; containers explicitly
stopped by the operator remain stopped. Remote bootstrap and alerts must work
without the laptop staying on.

If rollout fails, use the service-specific stop command in README.md, preserve
logs/volumes, disable only `constella-https` and `constella-http` with `a2dissite`,
then configtest and reload Apache. Retain the static IP, certificate and backup
while diagnosing. Do not delete the instance or volumes as a rollback step.
