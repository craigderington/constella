#!/bin/sh
# Craig runs this on the dedicated Lightsail host. No SSH or Git operations.
set -eu
umask 077
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
action=${1:-status}
backup=${2:-/var/backups/constella/v3-before-v5-20261005}
old_env="$repo/deploy/lightsail/production.env"
new_env="$repo/deploy/testnet-v5/cloud.env"
old() { docker compose --env-file "$old_env" -f deploy/lightsail/compose.yml "$@"; }
new() { docker compose --env-file "$new_env" -f deploy/testnet-v5/compose.cloud.yml "$@"; }
fail() { printf '%s\n' "$*" >&2; exit 1; }
need_backup() {
    [ -f "$backup/COMPLETE" ] || fail 'A completed v3 backup is required.'
    (cd "$backup" && sha256sum -c SHA256SUMS)
}

case "$action" in
backup)
    [ ! -e "$backup" ] || fail 'Backup destination already exists; do not overwrite it.'
    [ -f "$old_env" ] || fail 'Missing existing production.env; locate it before proceeding.'
    old config --quiet
    mkdir -p "$(dirname "$backup")"
    python3 - "$(dirname "$backup")" <<'PY'
import shutil, sys
usage = shutil.disk_usage(sys.argv[1])
if usage.free < max(4 * 1024**3, usage.total // 5):
    raise SystemExit('Keep at least 4 GiB and 20% disk headroom before backup/cutover.')
PY
    mkdir -m 700 "$backup"
    curl --fail --silent --show-error --max-time 15 http://127.0.0.1:3071/api/stats > "$backup/v3-stats.json"
    python3 - "$backup/v3-stats.json" <<'PY'
import json, sys
meta = json.load(open(sys.argv[1]))['meta']
if meta['chain_id'] != 'a8f4562e57e74f9d':
    raise SystemExit('Expected the existing v3 Explorer; nothing stopped.')
PY
    cp "$old_env" "$backup/production.env"
    cp deploy/lightsail/compose.yml "$backup/compose.v3.yml"
    node=$(old ps -q node)
    explorer=$(old ps -q explorer)
    postgres=$(old ps -q postgres)
    [ -n "$node" ] && [ -n "$explorer" ] && [ -n "$postgres" ] || fail 'Expected all three existing v3 services running.'
    node_image=$(docker inspect --format '{{.Image}}' "$node")
    explorer_image=$(docker inspect --format '{{.Image}}' "$explorer")
    postgres_image=$(docker inspect --format '{{.Image}}' "$postgres")
    printf '{"services":{"node":{"image":"%s"},"explorer":{"image":"%s"},"postgres":{"image":"%s"}}}\n' \
        "$node_image" "$explorer_image" "$postgres_image" > "$backup/images.override.json"
    volume=$(docker inspect --format '{{range .Mounts}}{{if eq .Destination "/data"}}{{.Name}}{{end}}{{end}}' "$node")
    [ "$volume" = constella-cloud-testnet_node-data ] || fail 'Unexpected v3 node volume; inspect before stopping.'
    docker image save "$node_image" "$explorer_image" "$postgres_image" > "$backup/images.tar"
    old stop -t 60 explorer node
    for container in "$node" "$explorer"; do
        state=$(docker inspect --format '{{.State.ExitCode}} {{.State.OOMKilled}}' "$container")
        [ "$state" = '0 false' ] || fail 'V3 shutdown was not clean; preserve logs and investigate.'
    done
    old exec -T postgres pg_dump -U constella -d explorer -Fc > "$backup/explorer.dump"
    old exec -T postgres pg_restore --list < "$backup/explorer.dump" > "$backup/dump-list.txt"
    docker run --rm --network none --read-only --entrypoint /bin/sh \
        --mount "type=volume,src=$volume,dst=/source,readonly" "$postgres_image" \
        -c 'cd /source && tar cf - .' > "$backup/node-data.tar"
    old logs --no-color --tail 300 node explorer postgres > "$backup/v3.log" 2>&1
    (cd "$backup" && sha256sum production.env compose.v3.yml images.override.json images.tar explorer.dump node-data.tar > SHA256SUMS)
    touch "$backup/COMPLETE"
    printf 'V3 node/Explorer stopped; database remains up. Private backup complete: %s\n' "$backup"
    ;;
start)
    need_backup
    [ -f "$new_env" ] || fail 'Prepare the private cloud.env first.'
    new config --quiet
    # A fresh chain must never adopt any previous rehearsal/experiment volume.
    for volume in constella-cloud-v5_node-data constella-cloud-v5_pgdata; do
        if docker volume inspect "$volume" >/dev/null 2>&1; then
            fail "Volume $volume already exists; inspect it. Use service-targeted start for an intentional resume."
        fi
    done
    for image in $(new config --images); do docker image inspect "$image" >/dev/null; done
    old stop -t 60 explorer node postgres
    new up -d --no-deps --no-build --pull never postgres node
    new up -d --no-build --pull never explorer
    new ps
    printf '%s\n' 'V5 started at fresh genesis. Wait for the homelab peers and ledger checks before releasing mining.'
    ;;
rollback)
    need_backup
    # Stop v5 first to release 7043/3071. Preserve every v5 volume and log.
    if [ -f "$new_env" ]; then new stop -t 60 explorer node postgres; fi
    docker image load -i "$backup/images.tar"
    docker compose --env-file "$backup/production.env" -f "$backup/compose.v3.yml" \
        -f "$backup/images.override.json" up -d --no-deps --no-build --pull never postgres node
    docker compose --env-file "$backup/production.env" -f "$backup/compose.v3.yml" \
        -f "$backup/images.override.json" up -d --no-build --pull never explorer
    printf '%s\n' 'V3 restored using retained volumes and saved images. Restore the homelab v3 peers separately.'
    ;;
status)
    new ps
    new logs --tail 25 node explorer
    curl --fail --silent --show-error --max-time 15 http://127.0.0.1:3071/api/stats
    df -h /var/lib/docker
    ;;
*) fail 'usage: sh deploy/testnet-v5/cloud-cutover.sh backup|start|rollback|status [private-backup-directory]' ;;
esac
