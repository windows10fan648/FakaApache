#!/usr/bin/env python3
"""Non-destructive security regression checks for a running instance."""

import argparse
import http.client
import sys
from pathlib import Path
from urllib.parse import urlsplit


def request(base_url, path, method="GET", headers=None):
    parsed = urlsplit(base_url)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=5)
    connection.request(method, path, headers=headers or {})
    response = connection.getresponse()
    body = response.read()
    connection.close()
    headers = {key.lower(): value for key, value in response.getheaders()}
    return response.status, headers, body


def expect(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    args = parser.parse_args()

    status, _, _ = request(args.url, "/%2e%2e/siteconfig.fakaapache")
    expect(status == 403, f"encoded traversal returned HTTP {status}")
    status, _, body = request(args.url, "/siteconfig.fakaapache")
    expect(status == 404, f"direct configuration request returned HTTP {status}")
    config_path = Path("siteconfig.fakaapache")
    if config_path.is_file():
        config_contents = config_path.read_bytes()
        expect(config_contents not in body, "direct configuration request exposed the configuration file")
    for sensitive_value in (b"listen_port", b"rate_limit_requests_per_minute", b"rate_limit_burst"):
        expect(sensitive_value not in body, "direct configuration request exposed sensitive configuration data")
    status, _, _ = request(args.url, "/%zz")
    expect(status == 400, f"malformed URL returned HTTP {status}")
    status, headers, _ = request(args.url, "/", "OPTIONS")
    expect(status == 405, f"unsupported method returned HTTP {status}")
    expect("get" in headers.get("allow", "").lower(), "Allow header is missing GET")
    status, _, _ = request(args.url, "/", headers={"Range": "bytes=999999999-"})
    expect(status == 416, f"invalid range returned HTTP {status}")
    status, headers, _ = request(args.url, "/")
    expect(status == 200, f"normal request returned HTTP {status}")
    expect(headers.get("x-content-type-options") == "nosniff", "security header is missing")
    print("Security smoke checks passed.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        print(f"Security smoke checks failed: {error}", file=sys.stderr)
        sys.exit(1)
