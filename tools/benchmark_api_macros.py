#!/usr/bin/env python3
"""Benchmark the /api/macros endpoint.

- No third-party deps (uses urllib).
- Reports payload sizes + latency stats (p50/p95/p99).

Usage:
  python3 tools/benchmark_api_macros.py --host 192.168.1.118
"""

from __future__ import annotations

import argparse
import json
import statistics
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from typing import Iterable, Optional


@dataclass(frozen=True)
class Sample:
    ok: bool
    ms: float
    bytes: int
    status: Optional[int]
    err: Optional[str]


def percentile(values: list[float], p: float) -> float:
    if not values:
        return float("nan")
    values_sorted = sorted(values)
    if p <= 0:
        return values_sorted[0]
    if p >= 100:
        return values_sorted[-1]
    k = (len(values_sorted) - 1) * (p / 100.0)
    f = int(k)
    c = min(f + 1, len(values_sorted) - 1)
    if f == c:
        return values_sorted[f]
    d0 = values_sorted[f] * (c - k)
    d1 = values_sorted[c] * (k - f)
    return d0 + d1


def fetch(url: str, timeout_s: float) -> Sample:
    t0 = time.perf_counter()
    try:
        req = urllib.request.Request(url, headers={"Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            status = getattr(resp, "status", None)
            data = resp.read()
        t1 = time.perf_counter()
        return Sample(ok=True, ms=(t1 - t0) * 1000.0, bytes=len(data), status=status, err=None)
    except urllib.error.HTTPError as e:
        t1 = time.perf_counter()
        return Sample(ok=False, ms=(t1 - t0) * 1000.0, bytes=0, status=e.code, err=f"HTTPError: {e}")
    except Exception as e:  # noqa: BLE001
        t1 = time.perf_counter()
        return Sample(ok=False, ms=(t1 - t0) * 1000.0, bytes=0, status=None, err=str(e))


def summarize(samples: Iterable[Sample]) -> dict:
    samples = list(samples)
    oks = [s for s in samples if s.ok]
    errs = [s for s in samples if not s.ok]

    lat = [s.ms for s in oks]
    sizes = [s.bytes for s in oks]

    out = {
        "requests": len(samples),
        "ok": len(oks),
        "errors": len(errs),
        "latency_ms": {
            "min": min(lat) if lat else None,
            "p50": percentile(lat, 50) if lat else None,
            "p95": percentile(lat, 95) if lat else None,
            "p99": percentile(lat, 99) if lat else None,
            "max": max(lat) if lat else None,
            "avg": (sum(lat) / len(lat)) if lat else None,
        },
        "payload_bytes": {
            "min": min(sizes) if sizes else None,
            "max": max(sizes) if sizes else None,
            "avg": (sum(sizes) / len(sizes)) if sizes else None,
        },
        "error_examples": [
            {"ms": e.ms, "status": e.status, "err": e.err} for e in errs[:5]
        ],
    }

    if oks:
        out["http_status_mode"] = statistics.mode([s.status for s in oks])

    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.118", help="Device IP/host")
    ap.add_argument("--path", default="/api/macros", help="Endpoint path")
    ap.add_argument("--https", action="store_true", help="Use https")
    ap.add_argument("--n", type=int, default=30, help="Number of measured requests")
    ap.add_argument("--warmup", type=int, default=3, help="Warmup requests (not counted)")
    ap.add_argument("--concurrency", type=int, default=1, help="Concurrent workers")
    ap.add_argument("--timeout", type=float, default=10.0, help="Per-request timeout seconds")
    ap.add_argument("--json", action="store_true", help="Output JSON only")
    args = ap.parse_args()

    scheme = "https" if args.https else "http"
    url = f"{scheme}://{args.host}{args.path}"

    # Warmup
    for _ in range(max(0, args.warmup)):
        fetch(url, args.timeout)

    # Measure
    samples: list[Sample] = []
    if args.concurrency <= 1:
        for _ in range(max(0, args.n)):
            samples.append(fetch(url, args.timeout))
    else:
        with ThreadPoolExecutor(max_workers=args.concurrency) as ex:
            futs = [ex.submit(fetch, url, args.timeout) for _ in range(max(0, args.n))]
            for f in as_completed(futs):
                samples.append(f.result())

    summary = {
        "url": url,
        "n": args.n,
        "warmup": args.warmup,
        "concurrency": args.concurrency,
        "timeout_s": args.timeout,
        "result": summarize(samples),
    }

    if args.json:
        print(json.dumps(summary, indent=2))
        return 0

    r = summary["result"]
    lat = r["latency_ms"]
    pay = r["payload_bytes"]

    print(f"URL: {url}")
    print(f"Requests: {r['requests']} ok={r['ok']} errors={r['errors']} concurrency={args.concurrency}")
    print(
        "Latency (ms): "
        f"min={lat['min']:.2f} p50={lat['p50']:.2f} p95={lat['p95']:.2f} p99={lat['p99']:.2f} max={lat['max']:.2f} avg={lat['avg']:.2f}"
        if lat["min"] is not None
        else "Latency (ms): (no successful samples)"
    )
    if pay["min"] is not None:
        print(f"Payload (bytes): min={pay['min']} max={pay['max']} avg={pay['avg']:.1f}")
    if r["errors"]:
        print("Errors (first 5):")
        for e in r["error_examples"]:
            print(f"  - {e['ms']:.2f}ms status={e['status']} err={e['err']}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
