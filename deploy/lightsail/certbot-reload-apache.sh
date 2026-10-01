#!/bin/sh
# Certbot deploy hook: load renewed certificates only after config validation.
set -eu
/usr/sbin/apache2ctl configtest
/usr/bin/systemctl reload apache2
