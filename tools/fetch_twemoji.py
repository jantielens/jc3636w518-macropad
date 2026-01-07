#!/usr/bin/env python3
"""Fetch Twemoji SVG assets and rasterize them to 64x64 PNGs.

This populates assets/icons_color/ with *true-color* icons (RGBA PNG) that are
compiled into firmware as LVGL TRUE_COLOR_ALPHA images.

Why Twemoji?
- Huge icon catalog (emoji)
- Easy SVG download per codepoint

Licensing:
- Twemoji code is MIT
- Twemoji graphics are CC BY 4.0 (attribution required)
  See: https://github.com/twitter/twemoji

Usage:
  python3 tools/fetch_twemoji.py --map-file tools/twemoji_icons.txt \
    --out-dir assets/icons_color --size 64

Map file format (UTF-8):
  <icon_id> <emoji>

Examples:
  emoji_play ▶️
  emoji_pause ⏸️
  emoji_ok ✅

Notes:
- Output filenames become C symbols (via tools/png2lvgl_assets.py), so icon_id
  must be a valid C identifier suffix (letters/digits/underscore; not starting
  with a digit).
- Some emoji include U+FE0F variation selector. If the exact Twemoji filename is
  missing, we retry after dropping FE0F.

Dependencies:
  python3 -m pip install --user cairosvg

"""

from __future__ import annotations

import argparse
import os
import re
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Iterable, List, Optional, Tuple

try:
    import cairosvg  # type: ignore
except Exception:
    cairosvg = None


_VALID_ICON_ID = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass(frozen=True)
class EmojiMapping:
    icon_id: str
    emoji: str


def _read_map_file(path: str) -> List[EmojiMapping]:
    out: List[EmojiMapping] = []
    with open(path, "r", encoding="utf-8") as f:
        for line_no, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue

            parts = line.split(maxsplit=1)
            if len(parts) != 2:
                raise SystemExit(
                    f"ERROR: Invalid map file line {line_no} in {path}. Expected: <icon_id> <emoji>\n"
                    f"  Got: {raw.rstrip()}"
                )

            icon_id, emoji = parts[0].strip(), parts[1].strip()
            if not _VALID_ICON_ID.match(icon_id):
                raise SystemExit(
                    "ERROR: icon_id must be a valid identifier (letters/digits/underscore; not starting with digit).\n"
                    f"  File: {path}:{line_no}\n"
                    f"  icon_id: {icon_id}"
                )
            if not emoji:
                raise SystemExit(f"ERROR: Empty emoji on {path}:{line_no}")

            out.append(EmojiMapping(icon_id=icon_id, emoji=emoji))

    # Dedup by icon_id (last wins)
    dedup: dict[str, EmojiMapping] = {}
    for m in out:
        dedup[m.icon_id] = m

    return [dedup[k] for k in sorted(dedup.keys())]


def _to_codepoints(s: str) -> List[int]:
    # Iterate Unicode code points. Python str is already codepoint-based.
    return [ord(ch) for ch in s]


def _codepoints_to_filename(cps: Iterable[int]) -> str:
    # Twemoji filenames are lowercase hex codepoints joined by '-'
    return "-".join(f"{cp:x}" for cp in cps)


def _download(url: str) -> bytes:
    try:
        with urllib.request.urlopen(url) as resp:
            return resp.read()
    except urllib.error.HTTPError as exc:
        raise exc
    except urllib.error.URLError as exc:
        raise SystemExit(f"ERROR: download failed for URL: {url}\n  {exc}")


def _try_download_svg(code: str, version: str) -> bytes:
    # Prefer a pinned tag for stability.
    url = f"https://raw.githubusercontent.com/twitter/twemoji/v{version}/assets/svg/{code}.svg"
    try:
        return _download(url)
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            raise FileNotFoundError(url)
        raise SystemExit(f"ERROR: download failed ({exc.code}) for URL: {url}")


def _svg_to_png(svg_bytes: bytes, out_path: str, size: int) -> None:
    if cairosvg is None:
        raise SystemExit(
            "ERROR: Python dependency 'cairosvg' is required to rasterize SVG to PNG.\n\n"
            "Install it with:\n"
            "  python3 -m pip install --user cairosvg\n"
        )

    cairosvg.svg2png(bytestring=svg_bytes, write_to=out_path, output_width=size, output_height=size)


def _drop_fe0f(codepoints: List[int]) -> List[int]:
    # U+FE0F variation selector-16
    return [cp for cp in codepoints if cp != 0xFE0F]


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch Twemoji SVGs and rasterize to PNG")
    ap.add_argument("--map-file", required=True, help="Mapping file: <icon_id> <emoji> per line")
    ap.add_argument("--out-dir", default="assets/icons_color", help="Output directory for PNGs")
    ap.add_argument("--size", type=int, default=64, help="PNG size in pixels (square)")
    ap.add_argument("--version", default="14.0.2", help="Twemoji version tag (default: 14.0.2)")
    ap.add_argument("--dry-run", action="store_true", help="Print intended downloads without writing files")
    ap.add_argument("--skip-missing", action="store_true", help="Skip missing emoji instead of failing")

    args = ap.parse_args()

    mappings = _read_map_file(args.map_file)
    if not mappings:
        print("No mappings found.", file=sys.stderr)
        return 2

    os.makedirs(args.out_dir, exist_ok=True)

    ok = 0
    missing: List[str] = []

    for m in mappings:
        cps = _to_codepoints(m.emoji)
        code = _codepoints_to_filename(cps)
        out_path = os.path.join(args.out_dir, f"{m.icon_id}.png")

        # Attempt download; retry without FE0F if missing.
        urls_tried: List[str] = []

        def attempt(code_str: str) -> Optional[bytes]:
            try:
                urls_tried.append(
                    f"https://raw.githubusercontent.com/twitter/twemoji/v{args.version}/assets/svg/{code_str}.svg"
                )
                return _try_download_svg(code_str, version=args.version)
            except FileNotFoundError:
                return None

        svg = attempt(code)
        if svg is None and 0xFE0F in cps:
            cps2 = _drop_fe0f(cps)
            code2 = _codepoints_to_filename(cps2)
            svg = attempt(code2)

        if svg is None:
            msg = "ERROR: Twemoji SVG not found for mapping:\n" f"  id: {m.icon_id}\n" f"  emoji: {m.emoji}\n" "  tried:\n" + "\n".join(
                f"    - {u}" for u in urls_tried
            )
            if args.skip_missing:
                print("! " + msg)
                missing.append(m.icon_id)
                continue
            raise SystemExit(msg)

        if args.dry_run:
            print(f"{m.icon_id} -> {out_path}  ({code})")
            ok += 1
            continue

        _svg_to_png(svg, out_path=out_path, size=args.size)
        ok += 1
        print(f"✓ {m.icon_id} -> {out_path}")

    print(f"✓ Twemoji icons written: {ok} to {args.out_dir}")
    if missing:
        print(f"! Skipped {len(missing)} missing icon(s):")
        for x in missing:
            print(f"  - {x}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
