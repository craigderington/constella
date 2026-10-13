#!/bin/sh
# Docker runtime only; no Constella containers or mining are started.
set -eu
[ "$(id -u)" = 0 ] || { echo 'Run this script with sudo.' >&2; exit 1; }
case "$(hostname -s)" in wolf359|gliese518|andromeda) ;; *) echo 'Unexpected host.' >&2; exit 1 ;; esac
. /etc/os-release
[ "$ID" = debian ] && [ "$VERSION_CODENAME" = trixie ] && [ "$(dpkg --print-architecture)" = amd64 ] || {
    echo 'This installer is for the three Debian 13 amd64 hosts.' >&2; exit 1;
}
for service in k3s k3s-agent; do
    if systemctl is-active --quiet "$service"; then
        echo "Stop and uninstall $service first." >&2; exit 1
    fi
done
if command -v k3s >/dev/null 2>&1 || pgrep -x k3s >/dev/null || [ -d /var/lib/rancher/k3s ]; then
    echo 'K3s removal is incomplete; inspect before installing Docker.' >&2; exit 1
fi
for package in docker.io docker-compose docker-doc podman-docker containerd runc; do
    if [ "$(dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null || true)" = installed ]; then
        echo "Conflicting package $package is installed; inspect before replacing it." >&2; exit 1
    fi
done
export DEBIAN_FRONTEND=noninteractive
apt-get -o DPkg::Lock::Timeout=120 -o APT::Update::Error-Mode=any update
apt-get -o DPkg::Lock::Timeout=120 install -y --no-install-recommends ca-certificates curl
install -m 0755 -d /etc/apt/keyrings
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
curl --retry 3 --connect-timeout 15 --max-time 120 -fsSL https://download.docker.com/linux/debian/gpg -o "$scratch/docker.asc"
cat > "$scratch/docker.sources" <<'EOF'
Types: deb
URIs: https://download.docker.com/linux/debian
Suites: trixie
Components: stable
Architectures: amd64
Signed-By: /etc/apt/keyrings/docker.asc
EOF
if [ -e /etc/apt/sources.list.d/docker.sources ] && ! cmp -s "$scratch/docker.sources" /etc/apt/sources.list.d/docker.sources; then
    echo 'Existing Docker repository differs; inspect before replacing it.' >&2; exit 1
fi
install -m 0644 "$scratch/docker.asc" /etc/apt/keyrings/docker.asc
install -m 0644 "$scratch/docker.sources" /etc/apt/sources.list.d/docker.sources
apt-get -o DPkg::Lock::Timeout=120 -o APT::Update::Error-Mode=any update
apt-get -o DPkg::Lock::Timeout=120 install -y --no-install-recommends \
    docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
systemctl enable --now docker.service containerd.service
systemctl is-active docker.service containerd.service
docker version
docker compose version
docker buildx version
docker run --rm --network none --memory 64m --cpus 0.25 --pids-limit 32 hello-world
docker ps --format '{{.Names}} {{.Status}}'
printf '\nDocker Engine and Compose installation verified on %s.\n' "$(hostname -s)"
