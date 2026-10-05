#!/bin/sh
# Craig runs this with sudo on Wolf359; K3s credentials remain root-only.
set -eu
umask 077
[ "$(id -u)" = 0 ] && [ "$(hostname -s)" = wolf359 ] || {
    echo 'Run with sudo on wolf359 only.' >&2; exit 1;
}
cd "$(dirname "$0")"
export KUBECONFIG=/etc/rancher/k3s/k3s.yaml
kctl() { k3s kubectl --request-timeout=30s "$@"; }
if [ "${1:-start}" = rollback ]; then
    kctl -n constella-testnet-v5 scale statefulset node8 --replicas=0
    kctl -n constella-testnet-v5 wait --for=delete pod/node8-0 --timeout=120s
    kctl -n constella-testnet scale statefulset node8 --replicas=1
    exit 0
fi
[ "${1:-start}" = start ] || { echo 'usage: wolf359-start.sh start|rollback' >&2; exit 1; }
python3 verify.py https://explorer.catasterism.xyz
sha256sum -c node-image.sha256
k3s ctr images import --platform linux/amd64 node-image.tar

existing=$(kctl get namespace constella-testnet-v5 --ignore-not-found -o name)
[ -z "$existing" ] || { echo 'V5 namespace already exists; inspect before resuming.' >&2; exit 1; }
mkdir -m 700 v3-backup
kctl -n constella-testnet get statefulset node8 -o yaml > v3-backup/node8.yml
pv=$(kctl -n constella-testnet get pvc data-node8-0 -o 'jsonpath={.spec.volumeName}')
path=$(kctl get pv "$pv" -o 'jsonpath={.spec.hostPath.path}')
case "$path" in /var/lib/rancher/k3s/storage/*) ;; *) echo 'Unexpected PVC path; inspect before stopping.' >&2; exit 1 ;; esac
kctl -n constella-testnet scale statefulset node8 --replicas=0
kctl -n constella-testnet wait --for=delete pod/node8-0 --timeout=120s
tar -C "$path" -cf v3-backup/node-data.tar .
sha256sum v3-backup/node-data.tar > v3-backup/node-data.sha256
# Keep the old namespace, StatefulSet and PVC for rollback.
kctl apply -f wolf359.yml
kctl -n constella-testnet-v5 get pods,pvc -o wide
printf '%s\n' 'V5 validator submitted with a fresh PVC. Mining remains disabled; verify logs and ledger height.'
