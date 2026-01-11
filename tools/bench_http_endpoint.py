#!/usr/bin/env python3
"""Tiny HTTP endpoint benchmark (stdlib only).

Examples:
  python3 tools/bench_http_endpoint.py --url http://192.168.1.118/api/macros -n 30 -c 1
  python3 tools/bench_http_endpoint.py --url http://192.168.1.118/api/macros -n 50 -c 10 --timeout 12

Notes:
- Reuses one HTTP connection per worker thread (keep-alive) when possible.
- Measures end-to-end latency (request -> fully read response body).
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import random
import statistics
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from threading import Thread
from typing import Any
from urllib.parse import urlparse

import http.client


@dataclass(frozen=True)
class Sample:
    ok: bool
    status: int | None
    ms: float
    bytes: int
    error: str | None = None


def _percentile(sorted_values: list[float], pct: float) -> float:
    if not sorted_values:
        return float("nan")
    if pct <= 0:
        return sorted_values[0]
    if pct >= 100:
        return sorted_values[-1]

    # Nearest-rank method
    k = int((pct / 100.0) * len(sorted_values))
    k = max(1, min(len(sorted_values), k))
    return sorted_values[k - 1]


def _worker(
    index: int,
    host: str,
    port: int,
    path: str,
    is_https: bool,
    timeout_s: float,
    jobs: queue.Queue[int],
    results: list[Sample],
    user_agent: str,
    jitter_ms: int,
) -> None:
    conn: http.client.HTTPConnection | http.client.HTTPSConnection | None = None

    def connect() -> http.client.HTTPConnection | http.client.HTTPSConnection:
        nonlocal conn
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass
        if is_https:
            conn = http.client.HTTPSConnection(host, port, timeout=timeout_s)
        else:
            conn = http.client.HTTPConnection(host, port, timeout=timeout_s)
        return conn

    connect()

    while True:
        try:
            job_id = jobs.get_nowait()
        except queue.Empty:
            break

        if jitter_ms > 0:
            time.sleep(random.random() * (jitter_ms / 1000.0))

        t0 = time.perf_counter()
        status: int | None = None
        body_len = 0
        err: str | None = None
        ok = False

        try:
            assert conn is not None
            conn.request(
                "GET",
                path,
                headers={
                    "Host": host,
                    "Connection": "keep-alive",
                    "Accept": "application/json",
                    "User-Agent": user_agent,
                },
            )
            resp = conn.getresponse()
            status = resp.status
            data = resp.read()  # read fully
            body_len = len(data)
            ok = 200 <= status < 300
        except Exception as e:
            err = repr(e)
            ok = False
            # Reconnect on any error; helps with broken keep-alive
            try:
                connect()
            except Exception:
                pass
        finally:
            t1 = time.perf_counter()

        results[job_id] = Sample(ok=ok, status=status, ms=(t1 - t0) * 1000.0, bytes=body_len, error=err)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("-n", "--requests", type=int, default=30)
    ap.add_argument("-c", "--concurrency", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--warmup", type=int, default=0)
    ap.add_argument("--jitter-ms", type=int, default=0, help="Random per-request delay up to this many ms (spreads bursts).")
    ap.add_argument("--json-out", default="", help="Write raw samples + summary JSON to this file.")
    args = ap.parse_args(argv)

    u = urlparse(args.url)
    if u.scheme not in ("http", "https"):
        print(f"Unsupported scheme: {u.scheme}", file=sys.stderr)
        return 2

    host = u.hostname
    if not host:
        print("URL missing hostname", file=sys.stderr)
        return 2

    port = u.port or (443 if u.scheme == "https" else 80)
    path = u.path or "/"
    if u.query:
        path += "?" + u.query

    if args.concurrency < 1:
        print("concurrency must be >= 1", file=sys.stderr)
        return 2
    if args.requests < 1:
        print("requests must be >= 1", file=sys.stderr)
        return 2

    user_agent = f"bench_http_endpoint.py/{os.getpid()}"

    # Warmup (single-threaded) to avoid first-hit artifacts
    if args.warmup > 0:
        conn = http.client.HTTPConnection(host, port, timeout=args.timeout) if u.scheme == "http" else http.client.HTTPSConnection(host, port, timeout=args.timeout)
        for _ in range(args.warmup):
            try:
                t0 = time.perf_counter()
                conn.request("GET", path, headers={"Host": host, "Connection": "keep-alive", "Accept": "application/json", "User-Agent": user_agent})
                resp = conn.getresponse()
                resp.read()
                t1 = time.perf_counter()
                print(f"warmup: status={resp.status} ms={(t1 - t0) * 1000.0:.2f}")
            except Exception as e:
                print(f"warmup error: {e!r}")
        try:
            conn.close()
        except Exception:
            pass

    jobs: queue.Queue[int] = queue.Queue()
    for i in range(args.requests):
        jobs.put(i)

    results: list[Sample] = [Sample(ok=False, status=None, ms=0.0, bytes=0, error="not run") for _ in range(args.requests)]

    threads: list[Thread] = []
    t0_all = time.perf_counter()
    for i in range(min(args.concurrency, args.requests)):
        t = Thread(
            target=_worker,
            args=(i, host, port, path, u.scheme == "https", args.timeout, jobs, results, user_agent, args.jitter_ms),
            daemon=True,
        )
        threads.append(t)
        t.start()

    for t in threads:
        t.join()
    t1_all = time.perf_counter()

    ok_samples = [s for s in results if s.ok]
    err_samples = [s for s in results if not s.ok]

    lat = sorted([s.ms for s in ok_samples])
    sizes = sorted([s.bytes for s in ok_samples])

    def fmt(v: float) -> str:
        return f"{v:.2f}"

    print(f"URL: {args.url}")
    print(f"Requests: {args.requests} ok={len(ok_samples)} errors={len(err_samples)} concurrency={args.concurrency}")
    if ok_samples:
        print(
            "Latency (ms): "
            f"min={fmt(lat[0])} "
            f"p50={fmt(_percentile(lat, 50))} "
            f"p95={fmt(_percentile(lat, 95))} "
            f"p99={fmt(_percentile(lat, 99))} "
            f"max={fmt(lat[-1])} "
            f"avg={fmt(sum(lat) / len(lat))}"
        )
        print(
            "Payload (bytes): "
            f"min={sizes[0]} p50={int(_percentile([float(x) for x in sizes], 50))} "
            f"p95={int(_percentile([float(x) for x in sizes], 95))} max={sizes[-1]} avg={int(sum(sizes) / len(sizes))}"
        )
    print(f"Total wall time: {(t1_all - t0_all):.2f}s")

    if err_samples:
        # Show a few distinct errors
        errs: dict[str, int] = {}
        for s in err_samples:
            key = s.error or "unknown"
            errs[key] = errs.get(key, 0) + 1
        print("Errors:")
        for k, v in sorted(errs.items(), key=lambda kv: kv[1], reverse=True)[:5]:
            print(f"  {v}x {k}")

    if args.json_out:
        out = {
            "meta": {
                "url": args.url,
                "requests": args.requests,
                "concurrency": args.concurrency,
                "timeout": args.timeout,
                "warmup": args.warmup,
                "jitter_ms": args.jitter_ms,
                "timestamp": datetime.now(timezone.utc).isoformat(),
                "total_wall_s": (t1_all - t0_all),
            },
            "samples": [asdict(s) for s in results],
        }
        os.makedirs(os.path.dirname(args.json_out) or ".", exist_ok=True)
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=2)
        print(f"Wrote: {args.json_out}")

    return 0 if len(err_samples) == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
