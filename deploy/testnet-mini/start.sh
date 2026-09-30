#!/bin/sh
# Headless startup for the dedicated testnet VM and its single node.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
"$base/colima/bin/colima" start constella-gate --activate=false --ssh-config=false
# Colima installs the LAN alias after guest provisioning, so repair it here,
# after `start` has finished. Keep the listener's /32 and the real LAN route.
"$base/colima/bin/colima" ssh --profile constella-gate -- sudo sh -c '
  if ip -4 -o addr show dev lo | grep -Fq "192.168.1.123/24"; then
    ip addr del 192.168.1.123/24 dev lo
    ip addr add 192.168.1.123/32 dev lo
  fi
'
docker --context colima-constella-gate compose --env-file "$base/mini.env" \
  -p constella-gate-mini -f "$base/deploy/testnet-mini/compose.yml" up -d --no-deps node6
