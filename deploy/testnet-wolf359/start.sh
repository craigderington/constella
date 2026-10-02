#!/bin/sh
# Craig runs this on wolf359: its existing K3s admin credentials require sudo.
# No Docker installation, cluster reconfiguration or production access.
set -eu
[ "$(id -u)" = 0 ] || { echo 'Run with sudo on wolf359.' >&2; exit 1; }
[ "$(hostname -s)" = wolf359 ] || { echo 'This bundle is only for wolf359.' >&2; exit 1; }
cd "$(dirname "$0")"
export KUBECONFIG=/etc/rancher/k3s/k3s.yaml
kctl() { k3s kubectl --request-timeout=30s "$@"; }

# Validate the actual host and storage provider before importing or applying.
node_ips=$(kctl get node wolf359 -o 'jsonpath={range .status.addresses[?(@.type=="InternalIP")]}{.address}{" "}{end}')
# Dual-stack nodes report both IPv4 and IPv6 InternalIP entries.
case " $node_ips " in
  *' 192.168.1.205 '*) ;;
  *) printf 'Unexpected wolf359 internal addresses: %s\n' "$node_ips" >&2; exit 1 ;;
esac
node_label=$(kctl get node wolf359 -o 'jsonpath={.metadata.labels.kubernetes\.io/hostname}')
[ "$node_label" = wolf359 ] || { echo 'Unexpected hostname selector label.' >&2; exit 1; }
node_ready=$(kctl get node wolf359 -o 'jsonpath={.status.conditions[?(@.type=="Ready")].status}')
[ "$node_ready" = True ] || { echo 'wolf359 is not Ready.' >&2; exit 1; }
provisioner=$(kctl get storageclass local-path -o 'jsonpath={.provisioner}')
[ "$provisioner" = rancher.io/local-path ] || { echo 'Unexpected storage provisioner.' >&2; exit 1; }

# This immutable bundle contains the already-tested amd64 image.
printf '%s  %s\n' \
  eca409563dd56eea36bdc9a65caa9b2ee3470773103267ed1630650cfff4c934 \
  constella-burst-fix-amd64-k3s-20261002.tar | sha256sum -c -
k3s ctr images import --platform linux/amd64 constella-burst-fix-amd64-k3s-20261002.tar

# Refuse to take over an unrelated namespace. Reruns target only our resources.
existing=$(kctl get namespace constella-testnet --ignore-not-found -o name)
if [ -n "$existing" ]; then
  owner=$(kctl get namespace constella-testnet -o 'jsonpath={.metadata.labels.app\.kubernetes\.io/part-of}')
  [ "$owner" = constella-testnet ] || { echo 'Existing namespace is not owned by this deployment.' >&2; exit 1; }
fi
kctl apply --dry-run=server -f namespace.yml
kctl apply -f namespace.yml
kctl apply --dry-run=server -f node.yml
kctl apply -f node.yml
kctl -n constella-testnet get pods,pvc -o wide
printf '\nNode submitted with mining disabled. Follow synchronization with:\n'
printf '%s\n' 'sudo k3s kubectl -n constella-testnet logs -f node8-0 --tail=30'
printf '%s\n' 'Readiness only checks the P2P listener; compare chain height/tip before mining.'
