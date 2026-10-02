# Project website on the dedicated Lightsail host

Craig runs these commands on production. Agents prepare and test locally only.
The apex, https://catasterism.xyz/, serves the static project page in `site/`.
The Explorer remains at https://explorer.catasterism.xyz/. No additional container,
application process or database is required. This launches the project website,
not mainnet. The legacy `deploy/finish-cloudfront.sh` is not used for this host.

## First launch

Push the prepared commits from the development machine yourself. Then on the
existing Debian Lightsail host (3.150.62.26), review the incoming changes and
update the checkout without resetting local production configuration:

```sh
cd /opt/constella
git status --short
git pull --ff-only
sudo apache2ctl -S
sudo bash deploy/lightsail/launch-site.sh
```

Stop if the pull reports conflicts or local changes that need resolution. Do not
reset or clean the production checkout. The script uses only the four public
files in `site/`; it never publishes repository files, env files or wallet data.
It does not invoke Docker or restart the node, Explorer or Postgres.

Prerequisites: the existing Apache/Certbot setup from the Explorer launch,
Python 3, working DNS, and ports 80/443 already open. The script checks that
catasterism.xyz resolves only to 3.150.62.26 and has no AAAA destination. A stale
IPv6 record must be removed or correctly configured before continuing. This
release covers the apex only; `www` can be added after its DNS is confirmed.

The script refuses conflicting apex vhosts or modified versions of its own
configuration templates. Review any such conflict before replacing a file.
It publishes to a new release directory, atomically switches the `current`
symlink, enables the ACME/redirect HTTP vhost, obtains a separate apex certificate,
then enables HTTPS. It preserves the existing Explorer certificate and vhost.
A dedicated certificate deploy hook validates/reloads Apache after renewal.
On failure it attempts to restore the prior website target and site enablement;
read its output and check Apache before retrying. Failed staging directories and
any issued certificate are retained for inspection.

## Verification

Expected: script ends with “Website published”, Apache configuration says
“Syntax OK”, and the apex page matches the source file. Verify from your browser
that the root site explains the project and its Explorer button opens the
separate live dashboard. Also run:

```sh
curl -I http://catasterism.xyz/
curl -I https://catasterism.xyz/
curl -fsS https://explorer.catasterism.xyz/api/stats
sudo certbot renew --cert-name catasterism.xyz --dry-run
systemctl list-timers certbot.timer
```

Expected HTTP 308 to the apex HTTPS URL, HTTPS 200, a valid trusted certificate,
Explorer JSON, successful apex renewal simulation, and a scheduled Certbot timer.
Explorer availability alone is not a chain/ledger health check: examine its latest
height, `updated_at`, `check` and `check_at` separately. The renewal simulation
checks ACME issuance; the hook is separately covered by Apache configtest/reload.
No new public port is needed. Google Fonts supplies the optional web fonts;
local fallback fonts keep the page readable if that service is unavailable.

## Rollback

For a first launch, disable only the two apex sites and reload Apache. This
leaves the Explorer running and retains all static releases and the certificate.
The server's previous default vhost may then answer the root domain.

```sh
sudo a2dissite catasterism-http catasterism-https
sudo apache2ctl configtest && sudo systemctl reload apache2
curl -fsS https://explorer.catasterism.xyz/api/stats
```

For a later website release, restore the previous static target (without a
reload). Run on Lightsail only if the saved target is nonempty and exists:

```sh
sudo bash <<'ROLLBACK'
set -euo pipefail
base=/var/www/catasterism
previous=$(cat "$base/previous-target")
[[ -n "$previous" && -f "$previous/index.html" ]]
ln -s "$previous" "$base/rollback-$$"
mv -Tf "$base/rollback-$$" "$base/current"
ROLLBACK
curl -I https://catasterism.xyz/
```

Each successful publish retains the previous directory. Do not prune releases
until the current one is verified and a rollback copy is retained. No database
restore or chain rollback belongs in this static website procedure.

## Local release checks, 2026-10-02

Apache 2.4 accepted the exact website and Explorer vhost templates together.
An isolated loopback container served the byte-identical page over HTTPS,
redirected HTTP with path/query preserved, served the ACME challenge exception,
returned the intended security/cache headers, and returned 404 for `.env`,
`CLAUDE.md` and `/deploy/`. Its one-day test certificate is local-only; production
issuance and renewal still require Craig's commands above.

Browser checks covered desktop at 1440 pixels and mobile at 390 and 320 pixels,
no horizontal page overflow, no duplicate IDs or broken fragment links, the
Explorer CTA, and the full sieve animation ending with the correct 48 wheel
residues. Light/mobile and dark/desktop axe checks reported no violations;
SVG contrast retains manual-review items, so this is not a blanket accessibility
certification. Browser JavaScript error output was empty. Shell syntax and Git
whitespace checks passed. Generated reports and screenshots are outside Git.
