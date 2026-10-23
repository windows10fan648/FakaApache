#!/usr/bin/env python3
"""Sustained concurrent health-check test for a running deployment."""

import argparse
import concurrent.futures
import http.client
import sys
import time
from urllib.parse import urlsplit


def fetch(url):
    parsed = urlsplit(url)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=10)
    connection.request("GET", parsed.path or "/healthz")
    response = connection.getresponse()
    body = response.read()
    connection.close()
    return response.status == 200 and body == b"ok\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8080/healthz")
    parser.add_argument("--duration", type=int, default=300, help="duration in seconds")
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--interval", type=float, default=0.1)
    args = parser.parse_args()
    if args.duration <= 0 or args.workers <= 0 or args.interval < 0:
        raise ValueError("duration and workers must be positive; interval cannot be negative")

    total = 0
    failures = 0
    deadline = time.monotonic() + args.duration
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        while time.monotonic() < deadline:
            batch = list(executor.map(fetch, [args.url] * args.workers))
            total += len(batch)
            failures += batch.count(False)
            if args.interval:
                time.sleep(args.interval)

    if failures:
        raise RuntimeError(f"{failures} of {total} soak requests failed")
    print(f"Soak test passed: {total} requests over {args.duration} seconds.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Soak test failed: {error}", file=sys.stderr)
        sys.exit(1)
