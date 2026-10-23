#!/usr/bin/env sh
set -eu

archive="${1:?usage: rollback.sh /path/to/fakaapache-YYYYmmddTHHMMSSZ.tar.gz}"
release="/opt/fakaapache/releases/rollback-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$release"
tar -xzf "$archive" -C "$release"
ln -sfn "$release/current" /opt/fakaapache/current
systemctl daemon-reload
systemctl restart fakaapache
curl --fail --silent http://127.0.0.1:8080/healthz >/dev/null
echo "Rollback completed: $release"
