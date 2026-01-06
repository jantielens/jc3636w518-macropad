#!/usr/bin/env python3
"""Minimal ESP32 partition table parser.

Reads an ESP-IDF/Arduino generated `*.partitions.bin` and prints offsets.
This avoids needing the Arduino ESP32 core tooling at site-generation time.

Partition entry format (32 bytes, little-endian):
- magic:   uint16 (0x50AA)
- type:    uint8
- subtype: uint8
- offset:  uint32
- size:    uint32
- label:   char[16]
- flags:   uint32

Table ends with a non-0x50AA magic (often 0xEBEB MD5 record or 0xFFFF/0x0000).
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass


ENTRY_STRUCT = struct.Struct("<HBBII16sI")
MAGIC_PARTITION = 0x50AA
MAGIC_MD5 = 0xEBEB


@dataclass(frozen=True)
class PartitionEntry:
    type: int
    subtype: int
    offset: int
    size: int
    label: str
    flags: int


def _decode_label(raw: bytes) -> str:
    raw = raw.split(b"\x00", 1)[0]
    return raw.decode("utf-8", errors="replace")


def read_partition_table(path: str) -> list[PartitionEntry]:
    with open(path, "rb") as f:
        data = f.read()

    entries: list[PartitionEntry] = []
    if len(data) < ENTRY_STRUCT.size:
        return entries

    for i in range(0, len(data), ENTRY_STRUCT.size):
        chunk = data[i : i + ENTRY_STRUCT.size]
        if len(chunk) < ENTRY_STRUCT.size:
            break

        (magic, ptype, subtype, offset, size, label_raw, flags) = ENTRY_STRUCT.unpack(chunk)

        if magic == MAGIC_MD5:
            break
        if magic != MAGIC_PARTITION:
            break

        entries.append(
            PartitionEntry(
                type=ptype,
                subtype=subtype,
                offset=offset,
                size=size,
                label=_decode_label(label_raw),
                flags=flags,
            )
        )

    return entries


def find_by_label(entries: list[PartitionEntry], label: str) -> PartitionEntry | None:
    for entry in entries:
        if entry.label == label:
            return entry
    return None


def find_preferred_app_partition(entries: list[PartitionEntry]) -> PartitionEntry | None:
    # Prefer common labels first.
    for label in ("app0", "factory", "ota_0"):
        entry = find_by_label(entries, label)
        if entry is not None:
            return entry

    # Fallback: pick the lowest-offset app partition.
    # ESP-IDF uses type 0x00 for app partitions.
    app_entries = [e for e in entries if e.type == 0x00]
    if not app_entries:
        return None
    return min(app_entries, key=lambda e: e.offset)


def main() -> int:
    parser = argparse.ArgumentParser(description="Parse ESP32 partition table binaries")
    parser.add_argument("partitions_bin", help="Path to app.ino.partitions.bin")
    parser.add_argument("--label", default="app0", help="Partition label to look up (default: app0)")
    parser.add_argument(
        "--print",
        choices=["offset-hex", "offset-dec", "csv"],
        default="offset-hex",
        help="Output format",
    )

    args = parser.parse_args()

    entries = read_partition_table(args.partitions_bin)
    if not entries:
        print("Error: no partition entries found", file=sys.stderr)
        return 2

    if args.print == "csv":
        print("# Name, Type, SubType, Offset, Size, Flags")
        for e in entries:
            print(
                f"{e.label},{e.type:#x},{e.subtype:#x},0x{e.offset:x},0x{e.size:x},0x{e.flags:x}",
            )
        return 0

    entry = find_by_label(entries, args.label)
    if entry is None:
        fallback = find_preferred_app_partition(entries)
        if fallback is None:
            labels = ", ".join(e.label for e in entries)
            print(
                f"Error: partition label not found: {args.label}. Available: {labels}",
                file=sys.stderr,
            )
            return 3

        # Keep stderr output minimal, but provide a hint for unusual layouts.
        print(
            f"Warning: partition label '{args.label}' not found; using app partition '{fallback.label}' @ 0x{fallback.offset:x}",
            file=sys.stderr,
        )
        entry = fallback

    if args.print == "offset-dec":
        print(str(entry.offset))
    else:
        print(f"0x{entry.offset:x}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
