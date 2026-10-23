#!/usr/bin/env sh
set -eu

destination="${1:-/var/backups/fakaapache}"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$destination"
tar -C /opt/fakaapache -czf "$destination/fakaapache-$timestamp.tar.gz" current
cp /etc/fakaapache/siteconfig.fakaapache "$destination/siteconfig-$timestamp.fakaapache"
find "$destination" -type f -mtime +14 -delete
echo "Backup written to $destination"
