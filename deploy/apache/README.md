# Serving catasterism.xyz

Two different problems, deliberately solved two different ways.

| Host | What it is | Where it lives | TLS from |
|---|---|---|---|
| `catasterism.xyz`, `www` | A static page. No server-side anything. | S3 + CloudFront | ACM (auto-renews, no cron) |
| `explorer.catasterism.xyz` | The Go explorer — a live process reading Postgres | A box you control, behind Apache | certbot / Let's Encrypt |

The static site needs no server, so it does not have one. Putting it on the
same box as the explorer would mean the marketing page goes down whenever the
node does, which is exactly backwards: the page is what people read *when* the
chain is having a bad day.

## The explorer box

The explorer binds `127.0.0.1:3071` and is never exposed directly. Apache is
the only public listener.

```bash
sudo a2enmod proxy proxy_http headers ssl rewrite
sudo cp explorer.catasterism.xyz.conf /etc/apache2/sites-available/
sudo a2ensite explorer.catasterism.xyz
sudo apache2ctl configtest && sudo systemctl reload apache2
```

Point DNS at the box **before** running certbot — HTTP-01 validation resolves
the name from the public internet, so a record that is not live yet fails with
an unhelpful error:

```bash
# in Route 53, zone Z07364351Y9Z8HB4HY25A
#   explorer.catasterism.xyz.  A  <the box's public IP>
dig +short explorer.catasterism.xyz     # must return the IP before continuing
```

Then:

```bash
sudo mkdir -p /var/www/certbot
sudo certbot --apache \
  -d explorer.catasterism.xyz \
  --agree-tos -m craigderington17@gmail.com --no-eff-email
```

Certbot writes its own `:443` vhost and rewires the `:80` one. Do not
hand-write an HTTPS block first — you end up with two vhosts on port 443 and
Apache silently serves whichever it parsed first.

### Renewal

The package installs a systemd timer; confirm it rather than assuming:

```bash
systemctl list-timers | grep certbot
sudo certbot renew --dry-run
```

The `ProxyPass /.well-known/acme-challenge/ !` line in the vhost is what keeps
renewal working. Without it the challenge request gets proxied to the Go
explorer, which returns its dashboard instead of the token, and renewal fails
90 days later when nobody is looking.

## Explorer-specific notes

- **The explorer is read-only and stateless-ish.** It follows a node over P2P
  and rebuilds everything in Postgres. Losing the box costs you an index, not
  the chain.
- **It answers `curl` with text and browsers with HTML.** Nothing in the proxy
  should rewrite `Accept` or `User-Agent`, or the text dashboard stops working.
- **Do not cache it at a CDN by default.** Height, tip and escrow change every
  few seconds; a cached explorer showing a stale tip is worse than a slow one.
  If it needs shielding, cache for no more than the share spacing (4 s).
- **Postgres stays on 5439 and must not be exposed.** Only Apache and the node
  should reach the box from outside, on 443 and 7043 respectively.

## If you would rather self-host the static site too

Keep CloudFront. But if you want one box for everything, serve the page from
Apache and drop the S3/CloudFront stack:

```apache
<VirtualHost *:443>
    ServerName catasterism.xyz
    ServerAlias www.catasterism.xyz
    DocumentRoot /var/www/catasterism
    <Directory /var/www/catasterism>
        Require all granted
        Options -Indexes
    </Directory>
    Header always set Strict-Transport-Security "max-age=31536000; includeSubDomains"
    Header always set X-Content-Type-Options "nosniff"
</VirtualHost>
```

and add `-d catasterism.xyz -d www.catasterism.xyz` to the certbot command.
The page is a single self-contained file — `site/index.html` in this repo, no
build step, no assets to sync.
