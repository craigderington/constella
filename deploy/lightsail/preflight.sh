#!/usr/bin/env bash
# READ ONLY. Craig runs this on the new dedicated host; no agent prod access.
set -u
printf '\nHost / operating system\n'
hostname
uname -m
nproc
cat /etc/os-release
printf '\nCapacity and load\n'
uptime
free -m
df -h /
if [ -d /var/lib/docker ]; then df -h /var/lib/docker; fi
printf '\nListeners (confirm 7043 and 3071 are available)\n'
ss -lnt
printf '\nClock and web services\n'
timedatectl show -p NTPSynchronized 2>/dev/null || true
systemctl is-active apache2 nginx docker 2>/dev/null || true
printf '\nDocker / existing workload (no environment or credentials)\n'
docker version --format '{{.Server.Version}}'
docker compose version
docker ps --format '{{.Names}}\t{{.Status}}\t{{.Image}}\t{{.Ports}}'
docker stats --no-stream --format '{{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.BlockIO}}'
docker system df
