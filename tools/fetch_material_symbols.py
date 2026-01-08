#!/usr/bin/env python3
"""Fetch Google Material Symbols and rasterize to 64x64 PNGs.

This is intended to populate assets/icons_mono/ with a large set of *monochrome*
icons that are tinted at runtime (alpha-mask pipeline).

We download SVGs from Google's static hosting (fonts.gstatic.com) using the
Material Symbols release URLs.

Example (filled icons, rounded family):
  python3 tools/fetch_material_symbols.py \
    --family rounded \
    --fill 1 \
    --size 64 \
    --out-dir assets/icons_mono \
    volume_up volume_off play_pause

You can also provide a file with icon names (one per line):
  python3 tools/fetch_material_symbols.py --names-file tools/material_icons.txt

Dependencies:
- Preferred: cairosvg (pure-Python package, but requires cairo libs on some OSes)
    python3 -m pip install --user cairosvg

If cairosvg is not available, the script errors with instructions.

Notes:
- Output PNGs are designed for the repo's alpha-mask converter (only the alpha
  channel matters). The SVG fill color doesn't matter as long as transparency is
  correct.
- This script does not vendor icons into git automatically; you decide what to commit.
"""

from __future__ import annotations

import argparse
import os
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Iterable, List


try:
    import cairosvg  # type: ignore
except Exception:
    cairosvg = None


@dataclass(frozen=True)
class SymbolRequest:
    name: str
    family: str  # outlined|rounded|sharp
    fill: int  # 0|1
    wght: int
    grad: int
    opsz: int


def _iter_names(args: argparse.Namespace) -> List[str]:
    names: List[str] = []

    if args.names_file:
        with open(args.names_file, "r", encoding="utf-8") as f:
            for line in f:
                s = line.strip()
                if not s:
                    continue
                if s.startswith("#"):
                    continue
                names.append(s)

    names.extend(args.names or [])

    # Dedup preserving order
    seen = set()
    out: List[str] = []
    for n in names:
        if n in seen:
            continue
        seen.add(n)
        out.append(n)
    return out


def _svg_url(req: SymbolRequest) -> str:
    # URL pattern used by Material Symbols (public static asset hosting).
    # Example:
    # https://fonts.gstatic.com/s/i/short-term/release/materialsymbolsrounded/volume_up/fill1/wght400/grad0/opsz48.svg
    fam = {
        "outlined": "materialsymbolsoutlined",
        "rounded": "materialsymbolsrounded",
        "sharp": "materialsymbolssharp",
    }[req.family]

    return (
        "https://fonts.gstatic.com/s/i/short-term/release/"
        f"{fam}/{req.name}/fill{req.fill}/wght{req.wght}/grad{req.grad}/opsz{req.opsz}.svg"
    )


def _svg_url_material_icons_round(name: str) -> str:
    # Older Material Icons (not Symbols) endpoint. This reliably serves the
    # rounded filled variant.
    # Example:
    # https://fonts.gstatic.com/s/i/materialiconsround/play_arrow/v6/24px.svg
    return f"https://fonts.gstatic.com/s/i/materialiconsround/{name}/v6/24px.svg"


def _download(url: str) -> bytes:
    try:
        with urllib.request.urlopen(url) as resp:
            return resp.read()
    except urllib.error.HTTPError as exc:
        raise SystemExit(f"ERROR: download failed ({exc.code}) for URL: {url}")
    except urllib.error.URLError as exc:
        raise SystemExit(f"ERROR: download failed for URL: {url}\n  {exc}")


def _download_symbol_with_fallback(req: SymbolRequest) -> bytes:
    url1 = _svg_url(req)
    try:
        with urllib.request.urlopen(url1) as resp:
            return resp.read()
    except urllib.error.HTTPError as exc:
        # Material Symbols hosting seems to move around; fall back to the older
        # Material Icons Round endpoint for rounded filled icons.
        if exc.code != 404:
            raise SystemExit(f"ERROR: download failed ({exc.code}) for URL: {url1}")

    url2 = _svg_url_material_icons_round(req.name)
    try:
        with urllib.request.urlopen(url2) as resp:
            return resp.read()
    except urllib.error.HTTPError as exc2:
        raise SystemExit(
            "ERROR: icon SVG not found in either source.\n"
            f"  Symbols URL: {url1}\n"
            f"  Icons URL:   {url2}\n"
            f"  HTTP: {exc2.code}\n"
        )
    except urllib.error.URLError as exc2:
        raise SystemExit(f"ERROR: download failed for URL: {url2}\n  {exc2}")


def _svg_to_png(svg_bytes: bytes, out_path: str, size: int) -> None:
    if cairosvg is None:
        raise SystemExit(
            "ERROR: Python dependency 'cairosvg' is required to rasterize SVG to PNG.\n"
            "\n"
            "Install it with:\n"
            "  python3 -m pip install --user cairosvg\n"
            "\n"
            "If installation fails due to missing system cairo libraries, install cairo\n"
            "for your OS and retry.\n"
        )

    cairosvg.svg2png(bytestring=svg_bytes, write_to=out_path, output_width=size, output_height=size)


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch Material Symbols and rasterize to PNG")
    ap.add_argument("names", nargs="*", help="Icon names (e.g. volume_up)")
    ap.add_argument("--names-file", help="File with icon names (one per line, '#' comments)")
    ap.add_argument("--out-dir", default="assets/icons_mono", help="Output directory for PNGs")

    ap.add_argument("--family", choices=["outlined", "rounded", "sharp"], default="rounded")
    ap.add_argument("--fill", type=int, choices=[0, 1], default=1, help="0=outline, 1=filled")
    ap.add_argument("--wght", type=int, default=400)
    ap.add_argument("--grad", type=int, default=0)
    ap.add_argument("--opsz", type=int, default=48)

    ap.add_argument("--size", type=int, default=64, help="Output PNG size in pixels (square)")
    ap.add_argument("--dry-run", action="store_true", help="Print URLs without downloading")
    ap.add_argument(
        "--skip-missing",
        action="store_true",
        help="Continue when an icon name is missing (404); prints a warning and skips it",
    )

    args = ap.parse_args()

    names = _iter_names(args)
    if not names:
        print("No icon names provided.", file=sys.stderr)
        return 2

    os.makedirs(args.out_dir, exist_ok=True)

    ok = 0
    missing: List[str] = []
    for name in names:
        req = SymbolRequest(
            name=name,
            family=args.family,
            fill=args.fill,
            wght=args.wght,
            grad=args.grad,
            opsz=args.opsz,
        )
        url = _svg_url(req)
        out_path = os.path.join(args.out_dir, f"{name}.png")

        if args.dry_run:
            # Show both primary and fallback URLs for transparency.
            print(url)
            print(_svg_url_material_icons_round(name))
            continue

        try:
            svg = _download_symbol_with_fallback(req)
        except SystemExit as exc:
            if not args.skip_missing:
                raise
            missing.append(name)
            print(f"! missing {name}: {exc}")
            continue

        _svg_to_png(svg, out_path=out_path, size=args.size)
        ok += 1
        print(f"✓ {name} -> {out_path}")

    if not args.dry_run:
        print(f"✓ Downloaded {ok} icon(s) to {args.out_dir}")
        if missing:
            print(f"! Skipped {len(missing)} missing icon(s):")
            for n in missing:
                print(f"  - {n}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
