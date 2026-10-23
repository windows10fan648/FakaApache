#!/usr/bin/env python3
"""Small repeatable concurrency smoke test for deployment validation."""

import argparse
import concurrent.futures
import http.client
import sys
from urllib.parse import urlsplit


def fetch(url):
    parsed = urlsplit(url)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=5)
    connection.request("GET", parsed.path or "/healthz")
    response = connection.getresponse()
    body = response.read()
    connection.close()
    return response.status, body


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8080/healthz")
    parser.add_argument("--requests", type=int, default=200)
    parser.add_argument("--workers", type=int, default=16)
    args = parser.parse_args()
    if args.requests <= 0 or args.workers <= 0:
        raise ValueError("requests and workers must be positive")

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        results = list(executor.map(fetch, [args.url] * args.requests))

    failures = [result for result in results if result != (200, b"ok\n")]
    if failures:
        raise RuntimeError(f"{len(failures)} of {args.requests} requests failed")
    print(f"Load test passed: {args.requests} requests with {args.workers} workers.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Load test failed: {error}", file=sys.stderr)
        sys.exit(1)
