#!/usr/bin/env python3
"""Poll a JSON endpoint until it responds or a deadline expires."""

from __future__ import annotations

import argparse
import json
import time
import urllib.error
import urllib.request


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Wait for a JSON endpoint to recover")
    parser.add_argument("--endpoint-name", required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--timeout-seconds", type=int, required=True)
    parser.add_argument("--retry-delay-seconds", type=int, required=True)
    parser.add_argument("--max-wait-seconds", type=int, required=True)
    return parser.parse_args()


def fetch_json(url: str, timeout_seconds: int) -> None:
    with urllib.request.urlopen(url, timeout=timeout_seconds) as response:
        payload = response.read().decode("utf-8")
    json.loads(payload)


def main() -> int:
    args = parse_args()
    start = time.monotonic()
    deadline = start + args.max_wait_seconds
    attempts = 0
    last_error = ""

    while True:
        attempts += 1
        try:
            fetch_json(args.url, args.timeout_seconds)
            elapsed_seconds = time.monotonic() - start
            print(
                json.dumps(
                    {
                        "ok": True,
                        "attempts": attempts,
                        "elapsed_seconds": round(elapsed_seconds, 3),
                        "max_wait_seconds": args.max_wait_seconds,
                        "url": args.url,
                        "reason": "",
                    },
                    indent=2,
                )
            )
            return 0
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, json.JSONDecodeError, ValueError) as exc:
            last_error = str(exc)

        now = time.monotonic()
        if now >= deadline:
            break
        if args.retry_delay_seconds > 0:
            time.sleep(min(args.retry_delay_seconds, max(0.0, deadline - now)))

    elapsed_seconds = time.monotonic() - start
    reason = (
        f"Timed out waiting for {args.endpoint_name} after {attempts} attempt(s) "
        f"(timeout={args.timeout_seconds}s, retryDelay={args.retry_delay_seconds}s, "
        f"boundedWait<={args.max_wait_seconds}s, elapsed={elapsed_seconds:.2f}s, url={args.url})."
    )
    if last_error:
        reason = f"{reason} Last error: {last_error}"

    print(
        json.dumps(
            {
                "ok": False,
                "attempts": attempts,
                "elapsed_seconds": round(elapsed_seconds, 3),
                "max_wait_seconds": args.max_wait_seconds,
                "url": args.url,
                "reason": reason,
            },
            indent=2,
        )
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
