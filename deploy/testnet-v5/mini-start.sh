#!/bin/sh
# Installed as /Users/cd/constella-public-v5/start-v5.sh at cutover.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
legacy=/Users/cd/constella-release-gate
"$legacy/colima/bin/colima" start constella-gate --activate=false --ssh-config=false
# Preserve the existing Colima LAN-route repair.
"$legacy/colima/bin/colima" ssh --profile constella-gate -- sudo sh -c '
  if ip -4 -o addr show dev lo | grep -Fq "192.168.1.123/24"; then
    ip addr del 192.168.1.123/24 dev lo
    ip addr add 192.168.1.123/32 dev lo
  fi
'
docker --context colima-constella-gate compose --env-file "$base/.env" \
    -f "$base/compose.yml" up -d --no-deps --no-build --pull never temperature node
