#!/usr/bin/env python3
"""Screen switch stress test for the web portal display API.

Goal
- Rapidly switch between on-device screens for a fixed duration (default: 5 minutes)
- Detect a likely "display/LVGL hang" without relying on serial logs

How hang detection works (API-only)
- Each screen switch request is sent via: PUT /api/display/screen {"screen":"..."}
- We then poll GET /api/info and wait for "current_screen" to match the requested id.
- If the device keeps serving HTTP but "current_screen" stops updating, the LVGL task is likely stuck
  (commonly in the flush/panel IO path).

Caveat
- This detects "LVGL task not applying screen switches".
- It cannot prove that pixels are updating on the panel in all failure modes.
  If you suspect a pure display transfer stall where the firmware still updates state,
  you may still want visual confirmation or add a dedicated on-device heartbeat counter.

This script is intentionally dependency-free (stdlib only).

Example
  python3 tools/screen_stress_test.py --host 192.168.1.123 --duration 300 --interval 0.10

With basic auth (STA/full mode)
  python3 tools/screen_stress_test.py --host 192.168.1.123 --auth user:pass
"""

from __future__ import annotations

import argparse
import base64
import csv
import json
import os
import random
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


DEFAULT_TIMEOUT_S = 3.0
DEFAULT_RETRIES = 2
DEFAULT_RETRY_SLEEP_S = 0.15


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _normalize_base_url(host: str) -> str:
    host = host.strip()
    if host.startswith("http://") or host.startswith("https://"):
        return host.rstrip("/")
    return f"http://{host.rstrip('/')}"


def _basic_auth_header(auth: Optional[str]) -> Dict[str, str]:
    if not auth:
        return {}
    if ":" not in auth:
        raise ValueError("--auth must be in the form user:pass")
    raw = auth.encode("utf-8")
    tok = base64.b64encode(raw).decode("ascii")
    return {"Authorization": f"Basic {tok}"}


def _http(
    base_url: str,
    method: str,
    path: str,
    *,
    body: Optional[bytes] = None,
    content_type: Optional[str] = None,
    accept: Optional[str] = None,
    timeout_s: float = DEFAULT_TIMEOUT_S,
    retries: int = DEFAULT_RETRIES,
    retry_sleep_s: float = DEFAULT_RETRY_SLEEP_S,
    extra_headers: Optional[Dict[str, str]] = None,
) -> Tuple[int, bytes]:
    url = f"{base_url}{path}"
    headers = {
        "User-Agent": "jc3636w518-screen-stress-test/1.0",
    }
    if accept:
        headers["Accept"] = accept
    if content_type:
        headers["Content-Type"] = content_type
    if extra_headers:
        headers.update(extra_headers)

    req = Request(url=url, data=body, headers=headers, method=method)

    last_err: Optional[BaseException] = None
    for attempt in range(1, max(1, retries) + 1):
        try:
            with urlopen(req, timeout=timeout_s) as resp:
                return resp.getcode(), resp.read()
        except HTTPError as e:
            try:
                data = e.read()
            except Exception:
                data = b""
            return e.code, data
        except (URLError, TimeoutError) as e:
            last_err = e
            if attempt < retries:
                time.sleep(retry_sleep_s * attempt)
                continue
            break

    raise RuntimeError(f"Request failed: {url} ({last_err})")


def get_info(base_url: str, *, headers: Dict[str, str], timeout_s: float, retries: int, retry_sleep_s: float) -> Dict[str, Any]:
    status, data = _http(
        base_url,
        method="GET",
        path="/api/info",
        accept="application/json",
        timeout_s=timeout_s,
        retries=retries,
        retry_sleep_s=retry_sleep_s,
        extra_headers=headers,
    )
    if status != 200:
        raise RuntimeError(f"GET /api/info failed: HTTP {status} body={data[:200]!r}")
    return json.loads(data.decode("utf-8", errors="replace"))


def get_health(base_url: str, *, headers: Dict[str, str], timeout_s: float, retries: int, retry_sleep_s: float) -> Dict[str, Any]:
    status, data = _http(
        base_url,
        method="GET",
        path="/api/health",
        accept="application/json",
        timeout_s=timeout_s,
        retries=retries,
        retry_sleep_s=retry_sleep_s,
        extra_headers=headers,
    )
    if status != 200:
        raise RuntimeError(f"GET /api/health failed: HTTP {status} body={data[:200]!r}")
    return json.loads(data.decode("utf-8", errors="replace"))


def set_screen(base_url: str, screen_id: str, *, headers: Dict[str, str], timeout_s: float, retries: int, retry_sleep_s: float) -> Tuple[int, str]:
    body = json.dumps({"screen": screen_id}, separators=(",", ":")).encode("utf-8")
    status, data = _http(
        base_url,
        method="PUT",
        path="/api/display/screen",
        body=body,
        content_type="application/json",
        accept="application/json",
        timeout_s=timeout_s,
        retries=retries,
        retry_sleep_s=retry_sleep_s,
        extra_headers=headers,
    )
    return status, data.decode("utf-8", errors="replace")


@dataclass
class SwitchResult:
    ts_iso: str
    requested: str
    applied: Optional[str]
    apply_latency_ms: Optional[int]
    http_status: int
    ok: bool
    note: str


def _extract_current_screen(info: Dict[str, Any]) -> Optional[str]:
    v = info.get("current_screen")
    if v is None:
        return None
    if isinstance(v, str):
        return v
    return None


def _discover_screens(info: Dict[str, Any]) -> List[str]:
    screens = info.get("available_screens")
    if not isinstance(screens, list):
        return []
    out: List[str] = []
    for s in screens:
        if not isinstance(s, dict):
            continue
        sid = s.get("id")
        if isinstance(sid, str) and sid:
            out.append(sid)
    return out


def write_csv(path: str, rows: List[SwitchResult]) -> None:
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ts_iso", "requested", "applied", "apply_latency_ms", "http_status", "ok", "note"])
        for r in rows:
            w.writerow([r.ts_iso, r.requested, r.applied, r.apply_latency_ms, int(r.http_status), int(r.ok), r.note])


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(description="Stress test rapid screen switching via /api/display/screen")
    p.add_argument("--host", required=True, help="Device IP/host (or full URL). Example: 192.168.1.111")
    p.add_argument("--duration", type=float, default=300.0, help="Duration in seconds (default: 300 = 5 min)")
    p.add_argument("--interval", type=float, default=0.10, help="Delay between switch requests in seconds (default: 0.10)")
    p.add_argument("--apply-timeout", type=float, default=2.0, help="Seconds to wait for /api/info current_screen to match (default: 2.0)")
    p.add_argument("--poll-interval", type=float, default=0.15, help="Polling interval for /api/info (default: 0.15)")
    p.add_argument("--consecutive-fail", type=int, default=3, help="Stop after N consecutive apply failures (default: 3)")
    p.add_argument("--screens", default=None, help="Comma-separated screen ids; if omitted, auto-discover from /api/info")
    p.add_argument("--mode", choices=("cycle", "random"), default="cycle", help="Switch order (default: cycle)")
    p.add_argument("--seed", type=int, default=12345, help="RNG seed for --mode random (default: 12345)")
    p.add_argument("--auth", default=None, help="Optional Basic Auth in form user:pass")
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S, help=f"HTTP timeout seconds (default: {DEFAULT_TIMEOUT_S})")
    p.add_argument("--retries", type=int, default=DEFAULT_RETRIES, help=f"Retries per request (default: {DEFAULT_RETRIES})")
    p.add_argument("--retry-sleep", type=float, default=DEFAULT_RETRY_SLEEP_S, help=f"Base sleep between retries (default: {DEFAULT_RETRY_SLEEP_S})")
    p.add_argument("--out", default=None, help="Optional CSV output path")

    args = p.parse_args(argv)

    base_url = _normalize_base_url(args.host)
    headers = _basic_auth_header(args.auth)

    # Probe device and discover screens.
    try:
        info0 = get_info(base_url, headers=headers, timeout_s=args.timeout, retries=args.retries, retry_sleep_s=args.retry_sleep)
    except Exception as e:
        print(f"ERROR: Failed to query /api/info: {e}", file=sys.stderr)
        return 1

    if not info0.get("has_display", False):
        print("ERROR: Device reports has_display=false; screen switching API won’t help.", file=sys.stderr)
        return 2

    if args.screens:
        screen_ids = [s.strip() for s in args.screens.split(",") if s.strip()]
    else:
        screen_ids = _discover_screens(info0)

    if not screen_ids:
        print("ERROR: No screens discovered from /api/info (available_screens empty?)", file=sys.stderr)
        return 2

    # Keep it deterministic unless explicitly random.
    rng = random.Random(int(args.seed))

    print(f"Target: {base_url}")
    print(f"Screens: {', '.join(screen_ids)}")
    print(f"Duration: {args.duration:.1f}s interval: {args.interval:.3f}s apply-timeout: {args.apply_timeout:.2f}s")
    print(f"Hang detection: stop after {args.consecutive_fail} consecutive apply failures")

    results: List[SwitchResult] = []
    consecutive_fail = 0

    start = time.monotonic()
    deadline = start + float(args.duration)

    i = 0
    next_idx = 0

    # Periodically probe /api/health to confirm the device is alive even if LVGL is stuck.
    next_health_probe = start
    last_health_ok = True

    while time.monotonic() < deadline:
        if args.mode == "random":
            requested = rng.choice(screen_ids)
        else:
            requested = screen_ids[next_idx]
            next_idx = (next_idx + 1) % len(screen_ids)

        t0 = time.monotonic()
        status, body = set_screen(base_url, requested, headers=headers, timeout_s=args.timeout, retries=args.retries, retry_sleep_s=args.retry_sleep)

        if status != 200:
            results.append(
                SwitchResult(
                    ts_iso=_now_iso(),
                    requested=requested,
                    applied=None,
                    apply_latency_ms=None,
                    http_status=int(status),
                    ok=False,
                    note=f"PUT failed: HTTP {status} body={body[:120]!r}",
                )
            )
            consecutive_fail += 1
        else:
            applied: Optional[str] = None
            ok = False
            note = ""

            # Poll /api/info until the LVGL task applies the pending screen.
            while True:
                if time.monotonic() - t0 > float(args.apply_timeout):
                    break
                try:
                    info = get_info(base_url, headers=headers, timeout_s=args.timeout, retries=args.retries, retry_sleep_s=args.retry_sleep)
                    applied = _extract_current_screen(info)
                except Exception as e:
                    note = f"/api/info error: {e}"
                    applied = None

                if applied == requested:
                    ok = True
                    break

                time.sleep(float(args.poll_interval))

            latency_ms = int((time.monotonic() - t0) * 1000.0)
            if not ok and not note:
                note = f"apply timeout; last current_screen={applied!r}"

            results.append(
                SwitchResult(
                    ts_iso=_now_iso(),
                    requested=requested,
                    applied=applied,
                    apply_latency_ms=latency_ms if ok else None,
                    http_status=int(status),
                    ok=ok,
                    note=note,
                )
            )

            consecutive_fail = 0 if ok else (consecutive_fail + 1)

        # Optional: confirm device is alive (even if LVGL stuck, health might still respond).
        now = time.monotonic()
        if now >= next_health_probe:
            next_health_probe = now + 5.0
            try:
                _ = get_health(base_url, headers=headers, timeout_s=args.timeout, retries=args.retries, retry_sleep_s=args.retry_sleep)
                last_health_ok = True
            except Exception:
                last_health_ok = False

        i += 1
        if i % 25 == 0:
            ok_count = sum(1 for r in results if r.ok)
            print(f"step {i}: ok={ok_count}/{len(results)} consecutive_fail={consecutive_fail} health_ok={int(last_health_ok)}")

        if consecutive_fail >= int(args.consecutive_fail):
            print("\nLikely hang detected: screen switches not being applied.")
            print(f"Last result: {results[-1]}")
            print(f"health_ok={last_health_ok} (HTTP responsiveness check)")
            break

        # Keep hammering.
        time.sleep(float(args.interval))

    # Summary
    total = len(results)
    ok_total = sum(1 for r in results if r.ok)
    fail_total = total - ok_total

    print("\nDone")
    print(f"Results: ok={ok_total} fail={fail_total} total={total}")

    if args.out:
        out_path = args.out
    else:
        ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        out_path = os.path.join("tools", f"screen_stress_{ts}.csv")

    try:
        write_csv(out_path, results)
        print(f"Wrote CSV: {out_path}")
    except Exception as e:
        print(f"WARNING: failed to write CSV {out_path}: {e}", file=sys.stderr)

    # Non-zero if we think we hit a hang.
    if consecutive_fail >= int(args.consecutive_fail):
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
