#!/bin/bash
# Craig runs this on Lightsail. Static website only; no Docker lifecycle changes.
set -Eeuo pipefail
[[ $EUID -eq 0 ]] || { echo 'Run with sudo bash deploy/lightsail/launch-site.sh' >&2; exit 1; }
cd -- "$(dirname -- "$0")/../.."
for tool in apache2ctl a2enmod a2ensite a2dissite certbot curl python3 install; do
    command -v "$tool" >/dev/null || { echo "Missing: $tool" >&2; exit 1; }
done
for file in index.html favicon.svg robots.txt sitemap.xml; do
    [[ -f site/$file && ! -L site/$file ]] || { echo "Missing regular site/$file" >&2; exit 1; }
done
# Deliberately apex-only. A stale AAAA record can break TLS issuance and visitors.
python3 - <<'PY'
import socket
expected = {'3.150.62.26'}
v4 = {x[4][0] for x in socket.getaddrinfo('catasterism.xyz', 443, socket.AF_INET)}
try:
    v6 = {x[4][0] for x in socket.getaddrinfo('catasterism.xyz', 443, socket.AF_INET6)}
except socket.gaierror as exc:
    if exc.errno not in (socket.EAI_NONAME, socket.EAI_NODATA):
        raise
    v6 = set()
if v4 != expected or v6:
    raise SystemExit(f'DNS preflight stopped: A={sorted(v4)}, AAAA={sorted(v6)}. Expected only 3.150.62.26, no AAAA.')
print('Apex DNS points to the intended IPv4 host; no IPv6 destination.')
PY
apache2ctl configtest
# Refuse to overwrite custom configurations or introduce a second apex vhost.
for proto in http https; do
    target="/etc/apache2/sites-available/catasterism-${proto}.conf"
    if [[ -e "$target" ]] && ! cmp -s "deploy/lightsail/site-${proto}.conf" "$target"; then
        echo "Review existing $target before launch; it differs from this template." >&2
        exit 1
    fi
done
for conf in /etc/apache2/sites-enabled/*.conf; do
    [[ -e "$conf" ]] || continue
    case "${conf##*/}" in catasterism-http.conf|catasterism-https.conf) continue;; esac
    if grep -Eiq '^[[:space:]]*Server(Name|Alias)[[:space:]]+([^#]*[[:space:]])?catasterism\.xyz([[:space:]]|$)' "$conf"; then
        echo "Existing apex virtual host in $conf; review before launch." >&2
        exit 1
    fi
done
# Existing Explorer certificate and vhost are not changed.
curl --noproxy '*' -fsS --max-time 20 --resolve explorer.catasterism.xyz:443:127.0.0.1 \
    https://explorer.catasterism.xyz/api/stats >/dev/null
base=/var/www/catasterism
install -d -m 0755 "$base/releases" /var/www/constella-acme/.well-known/acme-challenge
[[ ! -e "$base/current" || -L "$base/current" ]] || { echo 'current must be a symlink, not a directory' >&2; exit 1; }
release=$(mktemp -d "$base/releases/site-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
chmod 0755 "$release"
for file in index.html favicon.svg robots.txt sitemap.xml; do
    install -m 0644 "site/$file" "$release/$file"
done
previous=$(readlink "$base/current" || true)
http_was_enabled=0
https_was_enabled=0
[[ ! -e /etc/apache2/sites-enabled/catasterism-http.conf ]] || http_was_enabled=1
[[ ! -e /etc/apache2/sites-enabled/catasterism-https.conf ]] || https_was_enabled=1
# Save the rollback target outside DocumentRoot, never among public assets.
printf '%s\n' "$previous" > "$base/previous-target"
rollback_on_error() {
    code=$?
    trap - ERR
    set +e
    if [[ -n "$previous" ]]; then
        ln -s "$previous" "$base/rollback-$$"
        mv -Tf "$base/rollback-$$" "$base/current"
    elif [[ $(readlink "$base/current") == "$release" ]]; then
        rm -- "$base/current"
    fi
    [[ $http_was_enabled -eq 1 ]] || a2dissite catasterism-http
    [[ $https_was_enabled -eq 1 ]] || a2dissite catasterism-https
    apache2ctl configtest && systemctl reload apache2
    echo "Website launch failed; previous target/site enablement restored where possible. Review output; staged release retained: $release" >&2
    exit "$code"
}
trap rollback_on_error ERR
ln -s "$release" "$base/current-$$"
mv -Tf "$base/current-$$" "$base/current"
a2enmod ssl rewrite headers
install -m 0644 deploy/lightsail/site-http.conf /etc/apache2/sites-available/catasterism-http.conf
a2ensite catasterism-http
apache2ctl configtest
systemctl reload apache2
# This uses the existing Certbot account. Answer its prompts if it needs one.
certbot certonly --webroot -w /var/www/constella-acme \
    --cert-name catasterism.xyz -d catasterism.xyz
install -m 0644 deploy/lightsail/site-https.conf /etc/apache2/sites-available/catasterism-https.conf
a2ensite catasterism-https
install -d -m 0755 /etc/letsencrypt/renewal-hooks/deploy
install -m 0755 deploy/lightsail/certbot-reload-apache.sh \
    /etc/letsencrypt/renewal-hooks/deploy/catasterism-reload-apache
apache2ctl configtest
systemctl reload apache2
curl --noproxy '*' -fsS --max-time 20 --resolve catasterism.xyz:443:127.0.0.1 https://catasterism.xyz/ | cmp - site/index.html
curl --noproxy '*' -fsS --max-time 20 --resolve explorer.catasterism.xyz:443:127.0.0.1 \
    https://explorer.catasterism.xyz/api/stats >/dev/null
trap - ERR
printf '\nWebsite published: https://catasterism.xyz/\nRelease: %s\nPrevious target: %s\n' "$release" "${previous:-none (first launch)}"
echo 'Next: verify from your browser, then run the hostname-scoped renewal dry-run in WEBSITE.md.'
