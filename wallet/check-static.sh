#!/bin/sh
# A musl target alone does not guarantee a portable static artifact.
set -eu
headers=$(LC_ALL=C readelf -lW "$1")
dynamic=$(LC_ALL=C readelf -dW "$1")
case "$headers" in *INTERP*) echo 'Wallet unexpectedly requires a dynamic loader' >&2; exit 1;; esac
case "$dynamic" in *NEEDED*) echo 'Wallet unexpectedly requires shared libraries' >&2; exit 1;; esac
