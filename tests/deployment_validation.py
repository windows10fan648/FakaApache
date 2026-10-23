#!/usr/bin/env python3
"""Validate the externally visible behavior of a running FakaApache instance."""

import argparse
import http.client
import sys
from urllib.parse import urlsplit


def request(url, method="GET", headers=None):
    parsed = urlsplit(url)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=5)
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query
    connection.request(method, path, headers=headers or {})
    response = connection.getresponse()
    body = response.read()
    result = response.status, dict(response.getheaders()), body
    connection.close()
    return result


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    args = parser.parse_args()

    status, headers, body = request(args.url + "/healthz")
    check(status == 200 and body == b"ok\n", "health endpoint failed")
    check(headers.get("cache-control") == "no-store", "health endpoint is missing Cache-Control: no-store")

    status, headers, body = request(args.url + "/")
    check(status == 200, f"GET returned HTTP {status}")
    check(b"FakaApache Example Site" in body, "GET returned the wrong page")
    check(headers.get("x-content-type-options") == "nosniff", "security response header is missing")

    status, _, body = request(args.url + "/", "HEAD")
    check(status == 200 and not body, "HEAD returned an invalid response body")

    status, headers, body = request(args.url + "/", headers={"Range": "bytes=0-9"})
    check(status == 206 and len(body) == 10, "range response is invalid")
    check(headers.get("content-range", "").startswith("bytes 0-9/"), "Content-Range is missing")

    status, _, _ = request(args.url + "/missing-file.fakaapache")
    check(status == 404, f"missing file returned HTTP {status}")

    status, _, _ = request(args.url + "/%2e%2e/siteconfig.fakaapache")
    check(status == 403, f"traversal request returned HTTP {status}")

    print("Deployment validation passed.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        print(f"Deployment validation failed: {error}", file=sys.stderr)
        sys.exit(1)
