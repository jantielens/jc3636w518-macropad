#!/usr/bin/env python3
"""Memory test harness for ESP32 firmware.

Goals:
- Stream serial logs live while capturing to disk.
- Reboot device via portal API for a clean run.
- Run named scenarios that can:
  - extract [Mem] lines from serial
  - hit /api/health at checkpoints
  - pause for manual interaction
- Write artifacts in an agent-friendly format (raw + structured JSONL).

No external deps: uses stdlib + termios (Linux/macOS).
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import datetime as dt
import http.client
import json
import os
import queue
import re
import select
import subprocess
import sys
import termios
import threading
import time
import tty
import urllib.error
import urllib.request
import socket
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional


SCENARIOS: dict[str, dict[str, Any]] = {
    "s1": {
        "title": "Baseline boot + idle",
        "description": "Reboot, wait for IP + [Main] Setup complete, then capture first [Mem] hb.",
    },
    "s2": {
        "title": "Portal page load",
        "description": "Reboot, wait for readiness, then GET /, /network.html, /firmware.html and capture post-interaction [Mem] hb.",
    },
    "s6": {
        "title": "Browser-like portal load",
        "description": "Reboot, wait for readiness, then GET / + portal assets and fetch key /api/* endpoints in parallel (mimics a real browser first load).",
    },
    "prompt": {
        "title": "Manual interaction",
        "description": "Reboot, wait for readiness, then pause for manual device/UI interaction (no scripted HTTP steps).",
    },
    "s4": {
        "title": "Macros config POST/apply",
        "description": "Reboot, wait for readiness, then GET /api/macros and POST a minimal macros payload to /api/macros (exercises JSON parse/apply).",
    },
    "s5": {
        "title": "MQTT connect/publish",
        "description": "Reboot, wait for readiness, then observe mqtt_* tagged [Mem] snapshots (connect/discovery/first publish).",
    },
}

SCENARIO_ALIASES: dict[str, str] = {
    "boot": "s1",
    "s1_boot_idle": "s1",
    "s2_portal_load": "s2",
    "portal": "s2",
    "s6_browser": "s6",
    "browser": "s6",
    "manual": "prompt",
    "s4_macros": "s4",
    "macros": "s4",
    "s5_mqtt": "s5",
    "mqtt": "s5",
}


def _resolve_scenario(name: str) -> Optional[str]:
    key = (name or "").strip().lower()
    if not key:
        return None
    if key in SCENARIOS:
        return key
    return SCENARIO_ALIASES.get(key)


def _format_scenarios_for_cli() -> str:
    lines = ["Available scenarios:"]
    for key in sorted(SCENARIOS.keys()):
        meta = SCENARIOS[key]
        lines.append(f"  - {key}: {meta.get('title','')}")
        desc = meta.get("description", "")
        if desc:
            lines.append(f"      {desc}")
    lines.append("")
    lines.append("Examples:")
    lines.append("  python3 tools/memory_test_harness.py s2 192.168.1.112 --no-health --port /dev/ttyACM0")
    lines.append("  python3 tools/memory_test_harness.py s1 192.168.1.112 --no-health --port /dev/ttyACM0")
    return "\n".join(lines)


_MEM_RE = re.compile(
    r"^\[Mem\]\s+(?P<tag>\S+)\s+"
    r"hf=(?P<hf>\d+)\s+hm=(?P<hm>\d+)\s+hl=(?P<hl>\d+)\s+"
    r"hi=(?P<hi>\d+)\s+hin=(?P<hin>\d+)\s+frag=(?P<frag>\d+)\s+"
    r"pf=(?P<pf>\d+)\s+pm=(?P<pm>\d+)\s+pl=(?P<pl>\d+)"
)

_TRIPWIRE_RE = re.compile(
    r"^\[Mem\]\s+TRIPWIRE fired\s+tag=(?P<tag>\S+)\s+hin=(?P<hin>\d+)B\s+<\s+(?P<threshold>\d+)B"
)

_TASK_RE = re.compile(
    r"^\[Task\]\s+name=(?P<name>\S+)\s+prio=(?P<prio>\d+)\s+core=(?P<core>-?\d+)\s+stack_rem=(?P<stack_rem>\d+)B"
)

_ASYNCTCP_STACK_RE = re.compile(
    r"^\[Portal\]\s+AsyncTCP stack watermark:\s+task=(?P<task>\S+)\s+rem=(?P<rem_units>\d+)\s+units\s+\((?P<rem_bytes>\d+)\s+B\),\s+unit=(?P<unit_bytes>\d+)\s+B,\s+CONFIG_ASYNC_TCP_STACK_SIZE\(raw\)=(?P<cfg_bytes>\d+)"
)

_IP_RE = re.compile(r"Got IP:\s*(?P<ip>\d+\.\d+\.\d+\.\d+)")
_FW_RE = re.compile(r"Firmware:\s*v(?P<ver>[0-9A-Za-z._-]+)")
_ESP_ROM_RE = re.compile(r"^ESP-ROM:esp32", re.IGNORECASE)
_ESP_RST_RE = re.compile(r"^rst:", re.IGNORECASE)
_SETUP_COMPLETE_RE = re.compile(r"^\[Main\]\s+Setup complete\b")

# Crash markers (best-effort). We log a compact summary in derived metrics.
_PANIC_MARKERS: list[tuple[str, re.Pattern[str]]] = [
    ("guru_meditation", re.compile(r"Guru Meditation Error", re.IGNORECASE)),
    ("panic", re.compile(r"panic'ed", re.IGNORECASE)),
    ("stack_canary", re.compile(r"Stack canary watchpoint triggered", re.IGNORECASE)),
    ("backtrace", re.compile(r"^Backtrace:", re.IGNORECASE)),
    ("abort", re.compile(r"abort\(\)", re.IGNORECASE)),
    ("brownout", re.compile(r"brownout", re.IGNORECASE)),
]


def _now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def _ts_dirname(scenario: str) -> str:
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    safe = re.sub(r"[^A-Za-z0-9._-]+", "-", scenario).strip("-")
    return f"{stamp}_{safe}" if safe else stamp


def _best_effort_git_info() -> dict[str, Any]:
    def run(cmd: list[str]) -> str:
        try:
            out = subprocess.check_output(cmd, stderr=subprocess.DEVNULL)
            return out.decode("utf-8", errors="replace").strip()
        except Exception:
            return ""

    return {
        "commit": run(["git", "rev-parse", "--short", "HEAD"]),
        "branch": run(["git", "rev-parse", "--abbrev-ref", "HEAD"]),
        "status_porcelain": run(["git", "status", "--porcelain"]),
    }


def _auto_port() -> Optional[str]:
    for candidate in ("/dev/ttyACM0", "/dev/ttyUSB0"):
        if os.path.exists(candidate):
            return candidate
    return None


def _termios_baud(baud: int) -> int:
    mapping = {
        9600: termios.B9600,
        19200: termios.B19200,
        38400: termios.B38400,
        57600: termios.B57600,
        115200: termios.B115200,
        230400: termios.B230400,
        460800: termios.B460800,
        921600: termios.B921600,
    }
    if baud not in mapping:
        raise ValueError(f"Unsupported baud {baud}; supported: {sorted(mapping.keys())}")
    return mapping[baud]


class SerialFollower:
    def __init__(self, port: str, baud: int, raw_log_path: str, structured_log_path: str, mem_log_path: str, *, echo: bool = False):
        self.port = port
        self.baud = baud
        self.raw_log_path = raw_log_path
        self.structured_log_path = structured_log_path
        self.mem_log_path = mem_log_path
        self.task_log_path = os.path.join(os.path.dirname(mem_log_path), "tasks.jsonl")

        # Whether to mirror serial lines to stdout. Default off to keep terminal output
        # focused on harness logs; serial is always captured to artifacts.
        self.echo = echo

        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._fd: Optional[int] = None

        self.lines: "queue.Queue[str]" = queue.Queue()
        self.state_lock = threading.Lock()
        self.last_ip: Optional[str] = None
        self.firmware_version: Optional[str] = None

    def start(self) -> None:
        if self._thread is not None:
            return

        fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self._fd = fd

        attrs = termios.tcgetattr(fd)
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)

        # Input flags
        attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
        # Output flags
        attrs[1] &= ~termios.OPOST
        # Control flags
        attrs[2] &= ~(termios.CSIZE | termios.PARENB)
        attrs[2] |= termios.CS8
        # Local flags
        attrs[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)

        baud_const = _termios_baud(self.baud)
        # Some Python builds (notably certain minimal/embedded builds) do not expose
        # termios.cfsetispeed/cfsetospeed. The speed fields are indices 4/5.
        # attrs layout: [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
        try:
            termios.cfsetispeed(attrs, baud_const)
            termios.cfsetospeed(attrs, baud_const)
        except AttributeError:
            attrs[4] = baud_const
            attrs[5] = baud_const

        termios.tcsetattr(fd, termios.TCSANOW, attrs)

        self._thread = threading.Thread(target=self._run, name="serial-follower", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        if self._fd is not None:
            try:
                os.close(self._fd)
            except Exception:
                pass
            self._fd = None

    def _emit_structured(self, record: dict[str, Any]) -> None:
        record = {"ts": _now_iso(), **record}
        line = json.dumps(record, sort_keys=True)
        with open(self.structured_log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")

    def _run(self) -> None:
        assert self._fd is not None

        buf = b""
        with open(self.raw_log_path, "a", encoding="utf-8") as rawf, open(self.mem_log_path, "a", encoding="utf-8") as memf, open(self.task_log_path, "a", encoding="utf-8") as taskf:
            while not self._stop.is_set():
                r, _, _ = select.select([self._fd], [], [], 0.1)
                if not r:
                    continue
                try:
                    chunk = os.read(self._fd, 4096)
                except BlockingIOError:
                    continue
                except OSError:
                    continue
                if not chunk:
                    continue

                buf += chunk
                while b"\n" in buf:
                    line_bytes, buf = buf.split(b"\n", 1)
                    # Normalize CRLF
                    line_bytes = line_bytes.rstrip(b"\r")
                    try:
                        line = line_bytes.decode("utf-8", errors="replace")
                    except Exception:
                        line = repr(line_bytes)

                    # Live stream (optional)
                    if self.echo:
                        sys.stdout.write(line + "\n")
                        sys.stdout.flush()

                    # Raw log
                    rawf.write(line + "\n")
                    rawf.flush()

                    # In-memory queue
                    self.lines.put(line)

                    # Parse events
                    ipm = _IP_RE.search(line)
                    if ipm:
                        ip = ipm.group("ip")
                        with self.state_lock:
                            self.last_ip = ip
                        self._emit_structured({"type": "event", "event": "got_ip", "ip": ip})

                    fwm = _FW_RE.search(line)
                    if fwm:
                        ver = fwm.group("ver")
                        with self.state_lock:
                            self.firmware_version = ver
                        self._emit_structured({"type": "event", "event": "firmware", "version": ver})

                    mm = _MEM_RE.match(line)
                    if mm:
                        rec = {k: int(v) if k != "tag" else v for k, v in mm.groupdict().items()}
                        self._emit_structured({"type": "mem", **rec})
                        memf.write(json.dumps({"ts": _now_iso(), **rec}, sort_keys=True) + "\n")
                        memf.flush()

                    tw = _TRIPWIRE_RE.match(line)
                    if tw:
                        rec = {
                            "tag": tw.group("tag"),
                            "hin": int(tw.group("hin")),
                            "threshold": int(tw.group("threshold")),
                        }
                        self._emit_structured({"type": "tripwire", **rec})

                    tm = _TASK_RE.match(line)
                    if tm:
                        rec = {
                            "name": tm.group("name"),
                            "prio": int(tm.group("prio")),
                            "core": int(tm.group("core")),
                            "stack_rem": int(tm.group("stack_rem")),
                        }
                        self._emit_structured({"type": "task", **rec})
                        taskf.write(json.dumps({"ts": _now_iso(), **rec}, sort_keys=True) + "\n")
                        taskf.flush()

                    am = _ASYNCTCP_STACK_RE.match(line)
                    if am:
                        rec = {
                            "task": am.group("task"),
                            "rem_units": int(am.group("rem_units")),
                            "rem_bytes": int(am.group("rem_bytes")),
                            "unit_bytes": int(am.group("unit_bytes")),
                            "cfg_bytes": int(am.group("cfg_bytes")),
                        }
                        self._emit_structured({"type": "stack_watermark", "subsystem": "async_tcp", **rec})
                        taskf.write(json.dumps({"ts": _now_iso(), "type": "stack_watermark", "subsystem": "async_tcp", **rec}, sort_keys=True) + "\n")
                        taskf.flush()


@dataclass
class HarnessConfig:
    scenario: str
    ip: Optional[str]
    port: str
    baud: int
    out_dir: str
    reboot: bool
    auth: Optional[str]
    timeout_s: float
    pause: str
    interactive: bool
    health: bool
    pages: bool
    echo_serial: bool
    browser_parallelism: int
    browser_health_count: int


class Harness:
    def __init__(self, cfg: HarnessConfig):
        self.cfg = cfg

        scenario_key = _resolve_scenario(cfg.scenario) or cfg.scenario
        scenario_meta = SCENARIOS.get(scenario_key, {})

        run_dir = os.path.join(cfg.out_dir, _ts_dirname(cfg.scenario))
        os.makedirs(run_dir, exist_ok=True)
        self.run_dir = run_dir

        self.paths = {
            "serial": os.path.join(run_dir, "serial.log"),
            "structured": os.path.join(run_dir, "events.jsonl"),
            "mem": os.path.join(run_dir, "mem.jsonl"),
            "health": os.path.join(run_dir, "health.jsonl"),
            "tasks": os.path.join(run_dir, "tasks.jsonl"),
            "summary": os.path.join(run_dir, "summary.json"),
            "summary_md": os.path.join(run_dir, "summary.md"),
        }

        self.serial = SerialFollower(
            port=cfg.port,
            baud=cfg.baud,
            raw_log_path=self.paths["serial"],
            structured_log_path=self.paths["structured"],
            mem_log_path=self.paths["mem"],
            echo=cfg.echo_serial,
        )

        self._summary: dict[str, Any] = {
            "scenario": cfg.scenario,
            "scenario_key": scenario_key,
            "scenario_title": scenario_meta.get("title"),
            "scenario_description": scenario_meta.get("description"),
            "start_ts": _now_iso(),
            "port": cfg.port,
            "baud": cfg.baud,
            "initial_ip": cfg.ip,
            "git": _best_effort_git_info(),
            "artifacts": self.paths,
        }

    def _write_summary_md(self) -> None:
        d = self._summary.get("derived", {}) or {}
        lines: list[str] = []
        title = self._summary.get("scenario_title") or "Memory test run"
        key = self._summary.get("scenario_key") or self._summary.get("scenario")
        lines.append(f"# {title} ({key})")
        desc = self._summary.get("scenario_description")
        if desc:
            lines.append("")
            lines.append(desc)

        lines.append("")
        lines.append("## Run")
        lines.append(f"- Start: {self._summary.get('start_ts')}")
        lines.append(f"- End: {self._summary.get('end_ts')}")
        lines.append(f"- Firmware: v{self._summary.get('firmware_version')}")
        lines.append(f"- IP: {self._summary.get('final_ip')}")
        lines.append(f"- Serial: {self._summary.get('port')} @ {self._summary.get('baud')}")

        git = self._summary.get("git", {}) or {}
        if git.get("commit") or git.get("branch"):
            lines.append("")
            lines.append("## Git")
            if git.get("branch"):
                lines.append(f"- Branch: {git.get('branch')}")
            if git.get("commit"):
                lines.append(f"- Commit: {git.get('commit')}")
            if git.get("status_porcelain"):
                lines.append("- Dirty: yes")

        lines.append("")
        lines.append("## Derived")
        if d:
            hin = d.get("hin_min")
            hm = d.get("hm_min")
            pf = d.get("pf_min")
            pm = d.get("pm_min")
            frag = d.get("frag_max")

            lines.append(f"- Internal heap min (hin_min): {hin} B")
            lines.append(f"- Internal heap min-available (hm_min): {hm} B")
            lines.append(f"- PSRAM free min (pf_min): {pf} B")
            lines.append(f"- PSRAM min-available (pm_min): {pm} B")
            lines.append(f"- Max internal fragmentation (frag_max): {frag}%")
            tags = d.get("tags")
            if tags:
                lines.append(f"- Tags: {', '.join(tags)}")
        else:
            lines.append("- (no derived metrics; mem.jsonl empty)")

        trip = d.get("tripwire") if isinstance(d, dict) else None
        if isinstance(trip, dict) and trip.get("fired"):
            lines.append("")
            lines.append("## Tripwire")
            lines.append(
                f"- Fired: yes (tag={trip.get('tag')} hin={trip.get('hin')} B < {trip.get('threshold')} B)"
            )

            worst = trip.get("worst_stacks")
            if isinstance(worst, list) and worst:
                worst_str = ", ".join(f"{w.get('name')} {w.get('stack_rem')}B" for w in worst if isinstance(w, dict))
                if worst_str:
                    lines.append(f"- Worst stack margins: {worst_str}")

        panic = d.get("panic") if isinstance(d, dict) else None
        if isinstance(panic, dict) and panic.get("detected"):
            lines.append("")
            lines.append("## Panic")
            kind = panic.get("kind") or "unknown"
            line_no = panic.get("line_no")
            marker = panic.get("line")
            lines.append(f"- Detected: yes (kind={kind}, line={line_no})")
            if marker:
                lines.append(f"- Marker: {marker}")

        lines.append("")
        lines.append("## Artifacts")
        paths = self._summary.get("artifacts", {}) or {}
        for k in ("summary", "summary_md", "serial", "structured", "mem", "tasks", "health"):
            if k in paths:
                lines.append(f"- {k}: {paths[k]}")

        # Per-tag table for quick visual comparison.
        lines.append("")
        lines.append("## Mem snapshots")
        rows = self._load_mem_rows()
        if not rows:
            lines.append("- (no [Mem] snapshots captured)")
        else:
            lines.append("| tag | hf | hm | hl | hi | hin | frag% | pf | pm | pl |")
            lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
            for r in rows:
                lines.append(
                    "| {tag} | {hf} | {hm} | {hl} | {hi} | {hin} | {frag} | {pf} | {pm} | {pl} |".format(
                        tag=r.get("tag", ""),
                        hf=r.get("hf", ""),
                        hm=r.get("hm", ""),
                        hl=r.get("hl", ""),
                        hi=r.get("hi", ""),
                        hin=r.get("hin", ""),
                        frag=r.get("frag", ""),
                        pf=r.get("pf", ""),
                        pm=r.get("pm", ""),
                        pl=r.get("pl", ""),
                    )
                )

        with open(self.paths["summary_md"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

    def _load_mem_rows(self) -> list[dict[str, Any]]:
        try:
            with open(self.paths["mem"], "r", encoding="utf-8") as f:
                return [json.loads(line) for line in f if line.strip()]
        except Exception:
            return []

    def _auth_header(self) -> Optional[str]:
        if not self.cfg.auth:
            return None
        # auth form: user:pass
        token = base64.b64encode(self.cfg.auth.encode("utf-8")).decode("ascii")
        return f"Basic {token}"

    def http(
        self,
        method: str,
        url: str,
        *,
        json_body: Optional[dict[str, Any]] = None,
        timeout_s: float = 5.0,
        read_body: bool = True,
    ) -> tuple[int, dict[str, str], str]:
        data = None
        headers = {
            "User-Agent": "memory_test_harness/1.0",
        }
        ah = self._auth_header()
        if ah:
            headers["Authorization"] = ah

        if json_body is not None:
            data = json.dumps(json_body).encode("utf-8")
            headers["Content-Type"] = "application/json"

        req = urllib.request.Request(url=url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout_s) as resp:
                resp_headers = {k.lower(): v for (k, v) in (resp.headers.items() if resp.headers else [])}
                body = ""
                if read_body:
                    body = resp.read().decode("utf-8", errors="replace")
                return resp.status, resp_headers, body
        except urllib.error.HTTPError as e:
            resp_headers = {k.lower(): v for (k, v) in (e.headers.items() if e.headers else [])}
            body = ""
            if read_body and e.fp:
                body = e.read().decode("utf-8", errors="replace")
            return e.code, resp_headers, body
        except (TimeoutError, socket.timeout) as e:
            return 0, {}, f"timeout: {e}"
        except urllib.error.URLError as e:
            return 0, {}, f"urlerror: {e}"
        except Exception as e:
            return 0, {}, f"error: {e}"

    def capture_health(self, *, label: str) -> Optional[dict[str, Any]]:
        if not self.cfg.health:
            with open(self.paths["health"], "a", encoding="utf-8") as f:
                f.write(json.dumps({"ts": _now_iso(), "type": "health", "label": label, "skipped": True}, sort_keys=True) + "\n")
            return None
        ip = self.current_ip()
        if not ip:
            print("[harness] No IP known; cannot call /api/health")
            return None

        url = f"http://{ip}/api/health"
        status, headers, body = self.http("GET", url, timeout_s=5.0, read_body=True)
        rec: dict[str, Any] = {
            "ts": _now_iso(),
            "type": "health",
            "label": label,
            "ip": ip,
            "url": url,
            "http_status": status,
            "headers": headers,
            "body": body,
        }

        parsed = None
        if status == 200:
            try:
                parsed = json.loads(body)
                rec["json"] = parsed
            except Exception as e:
                rec["parse_error"] = str(e)

        with open(self.paths["health"], "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, sort_keys=True) + "\n")

        return parsed

    def current_ip(self) -> Optional[str]:
        # Prefer runtime-detected IP from serial after reboot
        with self.serial.state_lock:
            if self.serial.last_ip:
                return self.serial.last_ip
        return self.cfg.ip

    def wait_for(self, pattern: re.Pattern[str], *, timeout_s: float, label: str) -> Optional[str]:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            remaining = max(0.05, deadline - time.time())
            try:
                line = self.serial.lines.get(timeout=min(0.25, remaining))
            except queue.Empty:
                continue
            if pattern.search(line):
                return line
        print(f"[harness] Timeout waiting for {label}")
        return None

    def prompt(self, msg: str) -> None:
        if not self.cfg.interactive:
            print("[harness] NOTE: interactive prompts disabled; continuing")
            print(f"[harness] Would have prompted: {msg}")
            return
        print("\n[harness] ACTION REQUIRED")
        print(msg)
        input("[harness] Press Enter when done... ")

    def _pause_enabled(self, checkpoint: str) -> bool:
        if not self.cfg.pause:
            return False
        tokens = {t.strip().lower() for t in self.cfg.pause.split(",") if t.strip()}
        return ("all" in tokens) or (checkpoint.lower() in tokens)

    def maybe_pause(self, checkpoint: str, msg: str) -> None:
        if self._pause_enabled(checkpoint):
            self.prompt(msg)

    def capture_health_when_stable(
        self,
        *,
        label: str,
        settle_s: float = 2.0,
        max_wait_s: float = 30.0,
        delta_internal_free_bytes: int = 2048,
        delta_psram_free_bytes: int = 8192,
    ) -> Optional[dict[str, Any]]:
        """Capture /api/health when memory appears stable.

        We sample twice separated by `settle_s` and accept when both internal/psram
        free deltas are under thresholds. This is a pragmatic 'quiescent' signal
        without forcing a full heartbeat wait.
        """

        start = time.time()
        last = None
        while time.time() - start < max_wait_s:
            a = self.capture_health(label=f"{label}_a")
            if not a:
                return None
            time.sleep(settle_s)
            b = self.capture_health(label=f"{label}_b")
            if not b:
                return None

            try:
                ia = int(a.get("heap_internal_free", 0))
                ib = int(b.get("heap_internal_free", 0))
                pa = int(a.get("psram_free", 0))
                pb = int(b.get("psram_free", 0))
            except Exception:
                return b

            if abs(ib - ia) <= delta_internal_free_bytes and abs(pb - pa) <= delta_psram_free_bytes:
                return b
            last = b

        return last

    def reboot_fire_and_forget(self) -> bool:
        """Request reboot but do not wait for an HTTP response.

        For this firmware, reboot often happens immediately after the handler runs,
        which can tear down WiFi/TCP before a full HTTP response is received.
        We treat this call as fire-and-forget and confirm reboot via serial.
        """

        ip = self.cfg.ip
        if not ip:
            print("[harness] No IP provided; skipping reboot (need IP to call /api/reboot)")
            return False

        host = ip
        port = 80
        path = "/api/reboot"
        url = f"http://{host}{path}"
        print(f"[harness] POST {url} (fire-and-forget)")

        err: Optional[str] = None
        try:
            conn = http.client.HTTPConnection(host, port, timeout=0.5)
            headers = {"Connection": "close"}
            if self.cfg.auth:
                headers["Authorization"] = _basic_auth_header(self.cfg.auth)
            conn.request("POST", path, headers=headers)
            # Intentionally do NOT call getresponse(); device may reboot before replying.
            conn.close()
        except Exception as e:
            err = str(e)

        with open(self.paths["structured"], "a", encoding="utf-8") as f:
            f.write(
                json.dumps(
                    {"ts": _now_iso(), "type": "http", "mode": "fire_and_forget", "method": "POST", "url": url, "error": err},
                    sort_keys=True,
                )
                + "\n"
            )

        if err:
            print(f"[harness] NOTE: reboot request send error (may still reboot): {err}")
        return True

    def wait_for_reboot_marker(self, *, timeout_s: float) -> bool:
        """Best-effort detection of a reboot from serial output."""

        # Fast ROM banner / rst line tends to appear immediately on reboot.
        if self.wait_for(_ESP_ROM_RE, timeout_s=timeout_s, label="ESP-ROM"):
            return True
        if self.wait_for(_ESP_RST_RE, timeout_s=0.5, label="rst"):
            return True
        # Firmware boot log marker (later in boot).
        if self.wait_for(re.compile(r"System Boot"), timeout_s=0.5, label="System Boot"):
            return True
        return False

    def run(self) -> int:
        try:
            print(f"[harness] Artifacts: {self.run_dir}")
            print(f"[harness] Opening serial: {self.cfg.port} @ {self.cfg.baud}")

            scenario_key = self._summary.get("scenario_key") or self.cfg.scenario
            scenario_title = self._summary.get("scenario_title") or ""
            if scenario_title:
                print(f"[harness] Scenario: {scenario_key} - {scenario_title}")
            else:
                print(f"[harness] Scenario: {scenario_key}")
            self.serial.start()

            saw_reboot_marker = False

            self.maybe_pause(
                "serial_connected",
                "Serial is connected and streaming. If you want a clean capture, ensure your terminal is ready, then continue.",
            )

            # Reboot early for a clean run
            if self.cfg.reboot:
                self.reboot_fire_and_forget()
                # Confirm via serial (ROM banner / rst / System Boot). Only prompt if we see nothing.
                print("[harness] Waiting for reboot marker on serial...")
                saw_reboot_marker = self.wait_for_reboot_marker(timeout_s=min(20.0, self.cfg.timeout_s))
                if saw_reboot_marker:
                    print("[harness] Detected reboot via serial marker.")
                else:
                    self.prompt(
                        "Requested reboot via /api/reboot, but no reboot marker was seen on serial.\n"
                        "Press the device RESET button now (or power-cycle it), then press Enter to continue.\n"
                        "Tip: If you want to skip reboot entirely, rerun with --no-reboot."
                    )

            self.maybe_pause(
                "after_reboot",
                "Device reboot requested. If you need to re-seat USB or confirm the device rebooted, do it now.",
            )

            # Wait for boot marker (best-effort) and IP
            if not saw_reboot_marker:
                self.wait_for(re.compile(r"System Boot"), timeout_s=self.cfg.timeout_s, label="System Boot")
            self.wait_for(_IP_RE, timeout_s=self.cfg.timeout_s, label="Got IP")

            # Prefer an explicit readiness marker from the firmware before starting scenarios.
            # This is more reliable than time-based settling when --no-health is used.
            setup_line = self.wait_for(_SETUP_COMPLETE_RE, timeout_s=min(30.0, self.cfg.timeout_s), label="[Main] Setup complete")
            if setup_line:
                with open(self.paths["structured"], "a", encoding="utf-8") as f:
                    f.write(json.dumps({"ts": _now_iso(), "type": "ready", "marker": "setup_complete"}, sort_keys=True) + "\n")
            else:
                print("[harness] NOTE: did not see '[Main] Setup complete' marker; continuing")

            self.maybe_pause(
                "after_ip",
                "IP detected. If the portal requires user interaction (e.g., accept captive portal), do it now.",
            )

            # Scenario
            scenario = _resolve_scenario(self.cfg.scenario) or self.cfg.scenario.lower()
            rc = 0
            if scenario in ("s1", "s1_boot_idle", "boot"):
                rc = self._scenario_s1()
            elif scenario in ("s2", "s2_portal_load", "portal"):
                rc = self._scenario_s2()
            elif scenario in ("s6", "s6_browser", "browser"):
                rc = self._scenario_s6()
            elif scenario in ("s4", "macros", "s4_macros"):
                rc = self._scenario_s4()
            elif scenario in ("s5", "mqtt", "s5_mqtt"):
                rc = self._scenario_s5()
            elif scenario in ("prompt", "manual"):
                self.prompt("Perform the manual device interaction now.")
                rc = 0
            else:
                print(f"[harness] Unknown scenario: {self.cfg.scenario}")
                rc = 2

            self._summary["end_ts"] = _now_iso()
            self._summary["final_ip"] = self.current_ip()
            with self.serial.state_lock:
                self._summary["firmware_version"] = self.serial.firmware_version

            # Quick derived summary for agent convenience
            self._summary["derived"] = self._derive_metrics()

            with open(self.paths["summary"], "w", encoding="utf-8") as f:
                json.dump(self._summary, f, indent=2, sort_keys=True)

            # Human-readable companion summary
            self._write_summary_md()

            return rc
        finally:
            self.serial.stop()

    def _derive_metrics(self) -> dict[str, Any]:
        derived: dict[str, Any] = {}

        # Parse mem.jsonl for quick min summaries
        recs: list[dict[str, Any]] = []
        try:
            with open(self.paths["mem"], "r", encoding="utf-8") as f:
                recs = [json.loads(line) for line in f if line.strip()]
        except Exception:
            recs = []

        if recs:
            derived["hin_min"] = min(r.get("hin", 1 << 60) for r in recs)
            derived["hm_min"] = min(r.get("hm", 1 << 60) for r in recs)
            derived["pf_min"] = min(r.get("pf", 1 << 60) for r in recs)
            derived["pm_min"] = min(r.get("pm", 1 << 60) for r in recs)
            derived["frag_max"] = max(r.get("frag", 0) for r in recs)
            derived["tags"] = sorted({r.get("tag") for r in recs if r.get("tag")})

        # Parse events.jsonl for one-shot tripwire info (if present).
        tripwire: Optional[dict[str, Any]] = None
        try:
            with open(self.paths["structured"], "r", encoding="utf-8") as f:
                for line in f:
                    if not line.strip():
                        continue
                    try:
                        ev = json.loads(line)
                    except Exception:
                        continue
                    if ev.get("type") == "tripwire":
                        tripwire = ev
                        break
        except Exception:
            tripwire = None

        if tripwire:
            tw_summary: dict[str, Any] = {
                "fired": True,
                "ts": tripwire.get("ts"),
                "tag": tripwire.get("tag"),
                "hin": tripwire.get("hin"),
                "threshold": tripwire.get("threshold"),
            }

            # If we have tasks.jsonl, include a compact "worst stacks" snippet.
            try:
                task_rows: list[dict[str, Any]] = []
                with open(self.paths["tasks"], "r", encoding="utf-8") as tf:
                    for line in tf:
                        if not line.strip():
                            continue
                        try:
                            task_rows.append(json.loads(line))
                        except Exception:
                            continue
                tasks = [
                    r
                    for r in task_rows
                    if isinstance(r, dict) and "name" in r and "stack_rem" in r
                ]
                tasks_sorted = sorted(tasks, key=lambda r: int(r.get("stack_rem", 1 << 60)))
                tw_summary["worst_stacks"] = [
                    {"name": t.get("name"), "stack_rem": t.get("stack_rem"), "core": t.get("core"), "prio": t.get("prio")}
                    for t in tasks_sorted[:6]
                ]
            except Exception:
                pass

            derived["tripwire"] = tw_summary
        else:
            derived["tripwire"] = {"fired": False}

        # Best-effort crash detection from serial log.
        derived["panic"] = self._detect_panic_from_serial()

        return derived

    def _detect_panic_from_serial(self) -> dict[str, Any]:
        try:
            with open(self.paths["serial"], "r", encoding="utf-8") as f:
                for idx, line in enumerate(f, start=1):
                    s = line.rstrip("\n")
                    for kind, rx in _PANIC_MARKERS:
                        if rx.search(s):
                            return {
                                "detected": True,
                                "kind": kind,
                                "line": s,
                                "line_no": idx,
                            }
        except Exception as e:
            return {"detected": False, "error": str(e)}

        return {"detected": False}

    def _scenario_s1(self) -> int:
        # Baseline: capture a 'stable' health snapshot after IP, then wait for first heartbeat.
        self.capture_health_when_stable(label="s1_after_ip")
        self.wait_for(re.compile(r"\[Mem\]\s+hb\b"), timeout_s=90.0, label="[Mem] hb")
        self.capture_health_when_stable(label="s1_after_hb")
        return 0

    def _scenario_s2(self) -> int:
        # Portal load: capture health before and after loading key pages.
        self.capture_health_when_stable(label="s2_before")

        ip = self.current_ip()
        if not ip:
            print("[harness] No IP; cannot run S2 HTTP steps")
            return 1

        if self.cfg.pages:
            pages = ["/", "/network.html", "/firmware.html"]
            for p in pages:
                url = f"http://{ip}{p}"
                print(f"[harness] GET {url}")
                status, headers, _ = self.http("GET", url, timeout_s=5.0, read_body=False)
                content_length = headers.get("content-length")
                content_type = headers.get("content-type")
                content_encoding = headers.get("content-encoding")
                with open(self.paths["structured"], "a", encoding="utf-8") as f:
                    f.write(
                        json.dumps(
                            {
                                "ts": _now_iso(),
                                "type": "http",
                                "method": "GET",
                                "url": url,
                                "http_status": status,
                                "content_length": content_length,
                                "content_type": content_type,
                                "content_encoding": content_encoding,
                            },
                            sort_keys=True,
                        )
                        + "\n"
                    )
                time.sleep(1.0)

                self.maybe_pause(
                    f"after_get_{p}",
                    f"If you need to click something in the UI for {p} (e.g. open a modal), do it now, then continue.",
                )
        else:
            with open(self.paths["structured"], "a", encoding="utf-8") as f:
                f.write(json.dumps({"ts": _now_iso(), "type": "note", "note": "Skipped page GETs (pages disabled)"}, sort_keys=True) + "\n")

        # Give the device a moment to settle.
        time.sleep(2.0)

        # Ensure we capture at least one post-interaction memory snapshot in serial-first runs.
        # Without this, S2 can end before the next heartbeat and we only see boot/setup.
        # Firmware heartbeat is every 60s; allow enough time to catch it after setup.
        self.wait_for(re.compile(r"\[Mem\]\s+hb\b"), timeout_s=90.0, label="[Mem] hb (post S2)")

        self.capture_health_when_stable(label="s2_after")
        return 0

    def _scenario_s6(self) -> int:
        # Advanced browser-like flow:
        #  1) Full home load (/)
        #  2) Save settings + macros (no reboot)
        #  3) Full network page load (/network.html)
        self.capture_health_when_stable(label="s6_before")

        ip = self.current_ip()
        if not ip:
            print("[harness] No IP; cannot run S6 HTTP steps")
            return 1

        base = f"http://{ip}"

        self._log_note("S6: phase 1/3 - home page load")
        self._browser_get_static_bundle(base, page_path="/")
        self._browser_api_burst(
            base,
            ["/api/mode", "/api/config", "/api/info", "/api/macros", "/api/icons"],
            timeout_s=20.0,
            include_health_count=int(self.cfg.browser_health_count),
        )

        time.sleep(0.5)

        self._log_note("S6: phase 2/3 - save macros + config (no reboot)")
        if not self._save_macros_apply(base):
            self._log_note("S6: macros apply failed")
            return 1
        if not self._save_config_no_reboot(base):
            self._log_note("S6: config save (no reboot) failed")
            return 1

        time.sleep(0.5)

        self._log_note("S6: phase 3/3 - network page load")
        self._browser_get_static_bundle(base, page_path="/network.html")
        self._browser_api_burst(
            base,
            ["/api/mode", "/api/config", "/api/info"],
            timeout_s=20.0,
            include_health_count=int(self.cfg.browser_health_count),
        )

        # Give the device a moment to settle.
        time.sleep(2.0)

        # Ensure we capture at least one post-run memory snapshot.
        self.wait_for(re.compile(r"\[Mem\]\s+hb\b"), timeout_s=90.0, label="[Mem] hb (post S6)")
        self.capture_health_when_stable(label="s6_after")
        return 0

    def _log_http_event(self, *, method: str, url: str, status: int, headers: dict[str, str]) -> None:
        content_length = headers.get("content-length")
        content_type = headers.get("content-type")
        content_encoding = headers.get("content-encoding")
        with open(self.paths["structured"], "a", encoding="utf-8") as f:
            f.write(
                json.dumps(
                    {
                        "ts": _now_iso(),
                        "type": "http",
                        "method": method,
                        "url": url,
                        "http_status": status,
                        "content_length": content_length,
                        "content_type": content_type,
                        "content_encoding": content_encoding,
                    },
                    sort_keys=True,
                )
                + "\n"
            )

    def _log_note(self, note: str) -> None:
        with open(self.paths["structured"], "a", encoding="utf-8") as f:
            f.write(json.dumps({"ts": _now_iso(), "type": "note", "note": note}, sort_keys=True) + "\n")

    def _http_parallel_get(self, urls: list[str], *, timeout_s: float) -> list[tuple[str, int, dict[str, str]]]:
        """GET multiple URLs concurrently and return (url, status, headers) results."""

        if not urls:
            return []

        max_workers = max(1, int(self.cfg.browser_parallelism or 1))

        def worker(u: str) -> tuple[str, int, dict[str, str]]:
            status, headers, _ = self.http("GET", u, timeout_s=timeout_s, read_body=True)
            return (u, status, headers)

        results: list[tuple[str, int, dict[str, str]]] = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as ex:
            futures = [ex.submit(worker, u) for u in urls]
            for fut in concurrent.futures.as_completed(futures):
                try:
                    results.append(fut.result())
                except Exception:
                    results.append(("unknown", 0, {}))

        by_url = {u: (u, s, h) for (u, s, h) in results if u != "unknown"}
        return [by_url.get(u, (u, 0, {})) for u in urls]

    def _browser_get_static_bundle(self, base: str, *, page_path: str) -> None:
        # Fetch HTML + shared CSS/JS. This mirrors what a browser typically does on a fresh load.
        for p in (page_path, "/portal.css", "/portal.js"):
            url = f"{base}{p}"
            print(f"[harness] GET {url}")
            status, headers, _ = self.http("GET", url, timeout_s=10.0, read_body=True)
            self._log_http_event(method="GET", url=url, status=status, headers=headers)
            time.sleep(0.1)

    def _browser_api_burst(
        self,
        base: str,
        api_paths: list[str],
        *,
        timeout_s: float,
        include_health_count: int,
    ) -> None:
        paths = list(api_paths)
        if self.cfg.health:
            paths.extend(["/api/health"] * max(0, int(include_health_count)))
        urls = [f"{base}{p}" for p in paths]
        if not urls:
            return
        print(f"[harness] GET burst ({len(urls)} req, parallel={self.cfg.browser_parallelism})")
        burst = self._http_parallel_get(urls, timeout_s=timeout_s)
        for (u, status, headers) in burst:
            self._log_http_event(method="GET", url=u, status=status, headers=headers)

    def _save_config_no_reboot(self, base: str) -> bool:
        # Mimic portal.js saveOnly(): POST /api/config?no_reboot=1 with a subset of form fields.
        get_url = f"{base}/api/config"
        print(f"[harness] GET {get_url}")
        status, headers, body = self.http("GET", get_url, timeout_s=10.0, read_body=True)
        self._log_http_event(method="GET", url=get_url, status=status, headers=headers)
        if status != 200:
            return False

        try:
            doc = json.loads(body)
        except Exception:
            return False

        if not isinstance(doc, dict):
            return False

        allowed_fields = {
            "wifi_ssid",
            "wifi_password",
            "device_name",
            "fixed_ip",
            "subnet_mask",
            "gateway",
            "dns1",
            "dns2",
            "dummy_setting",
            "mqtt_host",
            "mqtt_port",
            "mqtt_username",
            "mqtt_password",
            "mqtt_interval_seconds",
            "basic_auth_enabled",
            "basic_auth_username",
            "basic_auth_password",
            "backlight_brightness",
            "screen_saver_enabled",
            "screen_saver_timeout_seconds",
            "screen_saver_fade_out_ms",
            "screen_saver_fade_in_ms",
            "screen_saver_wake_on_touch",
        }

        payload: dict[str, Any] = {}
        for k in allowed_fields:
            if k in doc:
                payload[k] = doc.get(k)

        # Never overwrite stored secrets in a harness run.
        for secret_field in ("wifi_password", "mqtt_password", "basic_auth_password"):
            if secret_field in payload:
                payload[secret_field] = ""

        post_url = f"{base}/api/config?no_reboot=1"
        print(f"[harness] POST {post_url} (save config, no reboot)")
        status, headers, _ = self.http("POST", post_url, json_body=payload, timeout_s=15.0, read_body=True)
        self._log_http_event(method="POST", url=post_url, status=status, headers=headers)
        return status == 200

    def _save_macros_apply(self, base: str) -> bool:
        # Similar to S4, but without screen-switch noise and without waiting for a heartbeat.
        get_url = f"{base}/api/macros"
        print(f"[harness] GET {get_url}")
        status, headers, body = self.http("GET", get_url, timeout_s=15.0, read_body=True)
        self._log_http_event(method="GET", url=get_url, status=status, headers=headers)
        if status != 200:
            return False

        try:
            doc = json.loads(body)
        except Exception:
            return False

        payload: dict[str, Any] = {}
        if isinstance(doc, dict):
            if isinstance(doc.get("defaults"), dict):
                payload["defaults"] = doc["defaults"]
            if isinstance(doc.get("screens"), list):
                payload["screens"] = doc["screens"]

        if "screens" not in payload:
            return False

        post_url = f"{base}/api/macros"
        print(f"[harness] POST {post_url} (macros apply)")
        status, headers, _ = self.http("POST", post_url, json_body=payload, timeout_s=20.0, read_body=True)
        self._log_http_event(method="POST", url=post_url, status=status, headers=headers)

        # Portal does a best-effort icon GC after saving macros.
        gc_url = f"{base}/api/icons/gc"
        print(f"[harness] POST {gc_url} (icons gc, best-effort)")
        gc_status, gc_headers, _ = self.http("POST", gc_url, timeout_s=10.0, read_body=True)
        self._log_http_event(method="POST", url=gc_url, status=gc_status, headers=gc_headers)

        return status == 200

    def _maybe_switch_screens(self) -> None:
        ip = self.current_ip()
        if not ip:
            return
        # Exercise LVGL screen switch path (and lvgl_switch_* tags) in a low-noise way.
        for screen_id in ("test", "info"):
            url = f"http://{ip}/api/display/screen"
            status, headers, _ = self.http("PUT", url, json_body={"screen": screen_id}, timeout_s=5.0, read_body=True)
            self._log_http_event(method="PUT", url=url, status=status, headers=headers)
            time.sleep(0.5)

    def _scenario_s4(self) -> int:
        # Macros POST/apply: GET /api/macros, derive a minimal payload, then POST it back.
        self.capture_health_when_stable(label="s4_before")

        ip = self.current_ip()
        if not ip:
            print("[harness] No IP; cannot run S4 HTTP steps")
            return 1

        get_url = f"http://{ip}/api/macros"
        print(f"[harness] GET {get_url}")
        status, headers, body = self.http("GET", get_url, timeout_s=10.0, read_body=True)
        self._log_http_event(method="GET", url=get_url, status=status, headers=headers)
        if status != 200:
            with open(self.paths["structured"], "a", encoding="utf-8") as f:
                f.write(json.dumps({"ts": _now_iso(), "type": "note", "note": f"GET /api/macros failed: {status}"}, sort_keys=True) + "\n")
            return 1

        try:
            doc = json.loads(body)
        except Exception as e:
            with open(self.paths["structured"], "a", encoding="utf-8") as f:
                f.write(json.dumps({"ts": _now_iso(), "type": "note", "note": f"GET /api/macros JSON parse failed: {e}"}, sort_keys=True) + "\n")
            return 1

        payload: dict[str, Any] = {}
        if isinstance(doc, dict):
            if isinstance(doc.get("defaults"), dict):
                payload["defaults"] = doc["defaults"]
            if isinstance(doc.get("screens"), list):
                payload["screens"] = doc["screens"]

        if "screens" not in payload:
            with open(self.paths["structured"], "a", encoding="utf-8") as f:
                f.write(json.dumps({"ts": _now_iso(), "type": "note", "note": "GET /api/macros response missing screens[]"}, sort_keys=True) + "\n")
            return 1

        post_url = f"http://{ip}/api/macros"
        print(f"[harness] POST {post_url} (macros apply)")
        status, headers, body = self.http("POST", post_url, json_body=payload, timeout_s=15.0, read_body=True)
        self._log_http_event(method="POST", url=post_url, status=status, headers=headers)
        if status != 200:
            with open(self.paths["structured"], "a", encoding="utf-8") as f:
                f.write(
                    json.dumps(
                        {"ts": _now_iso(), "type": "note", "note": f"POST /api/macros failed: {status}", "body": body[:512]},
                        sort_keys=True,
                    )
                    + "\n"
                )
            return 1

        # Exercise LVGL screen switches so we capture lvgl_switch_* tags.
        self._maybe_switch_screens()

        # Ensure we capture at least one post-interaction heartbeat.
        self.wait_for(re.compile(r"\[Mem\]\s+hb\b"), timeout_s=90.0, label="[Mem] hb (post S4)")
        self.capture_health_when_stable(label="s4_after")
        return 0

    def _scenario_s5(self) -> int:
        # MQTT connect/publish: check config presence, then watch serial for mqtt_* tags.
        self.capture_health_when_stable(label="s5_before")

        ip = self.current_ip()
        if not ip:
            print("[harness] No IP; cannot run S5 HTTP steps")
            return 1

        # Best-effort: check if MQTT is configured; if not, still run but note likely no mqtt_* tags.
        cfg_url = f"http://{ip}/api/config"
        status, headers, body = self.http("GET", cfg_url, timeout_s=10.0, read_body=True)
        self._log_http_event(method="GET", url=cfg_url, status=status, headers=headers)
        mqtt_host = ""
        if status == 200:
            try:
                j = json.loads(body)
                if isinstance(j, dict):
                    mqtt_host = str(j.get("mqtt_host") or "")
            except Exception:
                pass
        if not mqtt_host:
            with open(self.paths["structured"], "a", encoding="utf-8") as f:
                f.write(json.dumps({"ts": _now_iso(), "type": "note", "note": "MQTT host not configured (mqtt_* tags may not appear)"}, sort_keys=True) + "\n")

        # Exercise LVGL screen switches as part of the flow.
        self._maybe_switch_screens()

        # Wait for mqtt_* tags or at least one heartbeat.
        # We treat absence as non-fatal (device may not be configured for MQTT).
        self.wait_for(re.compile(r"\[Mem\]\s+mqtt_\S+\b"), timeout_s=30.0, label="[Mem] mqtt_* (best-effort)")
        self.wait_for(re.compile(r"\[Mem\]\s+hb\b"), timeout_s=90.0, label="[Mem] hb (post S5)")
        self.capture_health_when_stable(label="s5_after")
        return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="ESP32 memory instrumentation harness (serial + /api/health + scenarios)",
    )
    parser.add_argument("scenario", nargs="?", default=None, help="Scenario name (run without args to list)")
    parser.add_argument("ip", nargs="?", default=None, help="Device IP (optional; used for reboot/HTTP).")
    parser.add_argument("--port", default="auto", help="Serial port (default: auto -> /dev/ttyACM0 or /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--out", default="artifacts/memory-tests", help="Artifacts base directory")
    parser.add_argument("--no-reboot", action="store_true", help="Do not call POST /api/reboot")
    parser.add_argument("--auth", default=None, help="Basic auth in the form user:pass (optional)")
    parser.add_argument("--timeout", type=float, default=45.0, help="Timeout seconds for boot/IP waits")
    parser.add_argument(
        "--pause",
        default="",
        help=(
            "Comma-separated pause checkpoints (e.g. 'after_reboot,after_ip,all'). "
            "Also supports 'after_get_/' 'after_get_/network.html' 'after_get_/firmware.html'."
        ),
    )
    parser.add_argument("--non-interactive", action="store_true", help="Do not prompt for manual actions")
    parser.add_argument("--no-health", action="store_true", help="Do not call /api/health (reduces measurement perturbation)")
    parser.add_argument("--no-pages", action="store_true", help="Do not GET portal pages during S2")
    parser.add_argument("--browser-parallel", type=int, default=6, help="S6: parallelism for /api/* burst (default: 6)")
    parser.add_argument(
        "--browser-health-count",
        type=int,
        default=2,
        help="S6: number of /api/health fetches to include (default: 2; ignored with --no-health)",
    )
    parser.add_argument("--echo-serial", action="store_true", help="Echo serial logs to stdout (default: off; always captured to artifacts)")
    parser.add_argument(
        "--compare",
        nargs=2,
        metavar=("RUN_A", "RUN_B"),
        help="Compare two artifact run directories (each containing mem.jsonl) and print key deltas.",
    )

    args = parser.parse_args(argv)

    if args.compare:
        return compare_runs(args.compare[0], args.compare[1])

    if not args.scenario:
        print(_format_scenarios_for_cli())
        return 0

    resolved = _resolve_scenario(args.scenario)
    if resolved is None:
        print(f"Unknown scenario: {args.scenario}\n")
        print(_format_scenarios_for_cli())
        return 2

    port = args.port
    if port == "auto":
        port = _auto_port()
        if not port:
            print("[harness] Could not auto-detect serial port. Use --port /dev/ttyACM0")
            return 2

    cfg = HarnessConfig(
        scenario=resolved,
        ip=args.ip,
        port=port,
        baud=args.baud,
        out_dir=args.out,
        reboot=not args.no_reboot,
        auth=args.auth,
        timeout_s=args.timeout,
        pause=args.pause,
        interactive=not args.non_interactive,
        health=not args.no_health,
        pages=not args.no_pages,
        echo_serial=bool(args.echo_serial),
        browser_parallelism=int(args.browser_parallel),
        browser_health_count=int(args.browser_health_count),
    )

    h = Harness(cfg)
    return h.run()


def _load_mem_rows_from_dir(run_dir: str) -> list[dict[str, Any]]:
    mem_path = Path(run_dir) / "mem.jsonl"
    if not mem_path.exists():
        return []
    try:
        return [json.loads(line) for line in mem_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    except Exception:
        return []


def _last_by_tag(rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    out: dict[str, dict[str, Any]] = {}
    for r in rows:
        tag = r.get("tag")
        if tag:
            out[tag] = r
    return out


def compare_runs(run_a: str, run_b: str) -> int:
    rows_a = _load_mem_rows_from_dir(run_a)
    rows_b = _load_mem_rows_from_dir(run_b)

    by_tag_a = _last_by_tag(rows_a)
    by_tag_b = _last_by_tag(rows_b)
    tags = sorted(set(by_tag_a.keys()) | set(by_tag_b.keys()))

    if not tags:
        print("[compare] No mem.jsonl rows found in either run.")
        return 2

    def pick(row: dict[str, Any], k: str) -> Optional[int]:
        v = row.get(k)
        return int(v) if isinstance(v, int) else None

    def delta(a: Optional[int], b: Optional[int]) -> str:
        if a is None or b is None:
            return "n/a"
        return str(b - a)

    print(f"[compare] A: {run_a}")
    print(f"[compare] B: {run_b}")
    print("\nTag deltas (B - A):")
    print("- Columns: Δhin, Δhm, Δfrag, Δpf")

    for tag in tags:
        ra = by_tag_a.get(tag, {})
        rb = by_tag_b.get(tag, {})
        print(
            f"- {tag}: "
            f"Δhin={delta(pick(ra,'hin'), pick(rb,'hin'))}  "
            f"Δhm={delta(pick(ra,'hm'), pick(rb,'hm'))}  "
            f"Δfrag={delta(pick(ra,'frag'), pick(rb,'frag'))}  "
            f"Δpf={delta(pick(ra,'pf'), pick(rb,'pf'))}"
        )

    try:
        a_hin_min = min(int(r.get("hin")) for r in rows_a if isinstance(r.get("hin"), int))
        b_hin_min = min(int(r.get("hin")) for r in rows_b if isinstance(r.get("hin"), int))
        print("\nOverall:")
        print(f"- A hin_min: {a_hin_min} B")
        print(f"- B hin_min: {b_hin_min} B")
        print(f"- Δhin_min: {b_hin_min - a_hin_min} B")
    except Exception:
        pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
