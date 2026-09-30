#!/usr/bin/env bash
# Persistent local testnet relay; resolve the current container on each connection.
set -euo pipefail

mode=${1:-}
port=${2:-}
case "$port" in
  17043) node=node1 ;;
  17044) node=node2 ;;
  17045) node=node3 ;;
  *) echo "usage: $0 {listen|connect} {17043|17044|17045}" >&2; exit 2 ;;
esac

case "$mode" in
  listen)
    exec /usr/bin/socat "TCP-LISTEN:$port,bind=192.168.1.212,reuseaddr,fork" \
      "EXEC:/usr/bin/bash '$0' connect $port"
    ;;
  connect)
    ip=$(/usr/bin/docker inspect --format \
      '{{with index .NetworkSettings.Networks "constella-gate-liveheal"}}{{.IPAddress}}{{end}}' \
      "constella-gate-local-$node-1")
    if [[ ! "$ip" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
      echo "$node has no liveheal address yet" >&2
      exit 1
    fi
    exec /usr/bin/socat STDIO "TCP:$ip:7043,connect-timeout=5"
    ;;
  *) echo "unknown relay mode: $mode" >&2; exit 2 ;;
esac
