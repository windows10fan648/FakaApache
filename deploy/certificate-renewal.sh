#!/usr/bin/env sh
set -eu

# Run from a Certbot deploy hook after certificates have been renewed.
# Nginx terminates TLS; FakaApache remains an HTTP loopback backend.
nginx -t
systemctl reload nginx
