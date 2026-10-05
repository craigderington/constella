#!/bin/sh
# Craig runs this from his reviewed checkout on the dedicated cloud host.
set -eu
umask 077
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ ! -e deploy/testnet-v5/cloud.env ] || {
    echo 'cloud.env already exists; inspect the existing prepared release instead of overwriting it.' >&2
    exit 1
}
[ "$(uname -m)" = x86_64 ] || { echo 'This cloud profile requires AMD64.' >&2; exit 1; }
docker build -f deploy/testnet-v5-asus/Dockerfile.node -t constella:v5-release-20261005-amd64 .
docker build -f deploy/testnet-v5-asus/Dockerfile.explorer -t constella-explorer:v5-release-20261005-amd64 .
node=$(docker image inspect --format '{{.Id}}' constella:v5-release-20261005-amd64)
explorer=$(docker image inspect --format '{{.Id}}' constella-explorer:v5-release-20261005-amd64)
# Reuse the installed Postgres build, pinned by ID, with a NEW database volume.
postgres=$(docker inspect --format '{{.Image}}' constella-cloud-testnet-postgres-1)
docker image inspect "$postgres" >/dev/null
python3 - "$node" "$explorer" "$postgres" <<'PY'
from pathlib import Path
import os, secrets, sys
content = Path('deploy/testnet-v5/cloud.env.example').read_text()
values = dict(zip(('NODE_IMAGE', 'EXPLORER_IMAGE', 'POSTGRES_IMAGE'), sys.argv[1:]))
values['V5_DB_PASSWORD'] = secrets.token_hex(32)
lines = []
for line in content.splitlines():
    key = line.split('=', 1)[0]
    lines.append(key + '=' + values[key] if key in values else line)
fd = os.open('deploy/testnet-v5/cloud.env', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'w') as stream:
    stream.write('\n'.join(lines) + '\n')
PY
docker compose --env-file deploy/testnet-v5/cloud.env -f deploy/testnet-v5/compose.cloud.yml config --quiet
printf 'Prepared v5 images:\n%s\n%s\nPostgres: %s\n' "$node" "$explorer" "$postgres"
printf '%s\n' 'The existing v3 stack is unchanged. cloud.env is private and mode 0600.'
