#!/usr/bin/env python3
"""Map guest pread ranges from an FS trace to logical LBP asset names."""

from __future__ import annotations

import argparse
import re
import struct
from collections import defaultdict
from pathlib import Path

ENTRY_SIZE = 28
FARC_NAMES = {"intro_001.farc", "base_001.farc", "craftworld_001.farc", "chunk1_001.farc"}
TRACE = re.compile(
    r"\[LBP3_FS_TRACE\].*op=(\w+) path=\"([^\"]+)\" "
    r"size=(\d+) offset=(-?\d+) result=(-?\d+)"
)


def parse_filedb(path: Path) -> dict[tuple[bytes, int], list[str]]:
    data = path.read_bytes()
    if len(data) < 8:
        raise ValueError(f"truncated FileDB: {path}")
    revision, count = struct.unpack(">II", data[:8])
    if revision != 0x01480100:
        raise ValueError(f"unsupported FileDB revision 0x{revision:08x}")
    cursor = 8
    paths: dict[tuple[bytes, int], list[str]] = defaultdict(list)
    for index in range(count):
        if cursor + 2 > len(data):
            raise ValueError(f"FileDB entry {index} is truncated")
        path_size = int.from_bytes(data[cursor : cursor + 2], "big")
        cursor += 2
        end_path = cursor + path_size
        end_entry = end_path + 32
        if end_entry > len(data):
            raise ValueError(f"FileDB entry {index} extends past EOF")
        if path_size:
            name = data[cursor:end_path].decode("utf-8")
            entry = data[end_path:end_entry]
            payload_size = int.from_bytes(entry[4:8], "big")
            paths[(entry[8:28], payload_size)].append(name)
        cursor = end_entry
    if cursor != len(data):
        raise ValueError(f"FileDB has {len(data) - cursor} trailing bytes")
    return paths


def parse_farc(path: Path) -> list[tuple[bytes, int, int]]:
    size = path.stat().st_size
    with path.open("rb") as archive:
        archive.seek(size - 8)
        footer = archive.read(8)
        count_bytes, magic = footer[:4], footer[4:]
        if magic != b"FARC":
            raise ValueError(f"bad FARC footer: {path}")
        count = int.from_bytes(count_bytes, "big")
        table_size = count * ENTRY_SIZE
        table_start = size - 8 - table_size
        if table_start < 0:
            raise ValueError(f"invalid FARC table: {path}")
        archive.seek(table_start)
        table = archive.read(table_size)
        if len(table) != table_size:
            raise ValueError(f"short FARC table: {path}")
    entries = []
    for index in range(count):
        record = table[index * ENTRY_SIZE : (index + 1) * ENTRY_SIZE]
        offset, payload_size = struct.unpack(">II", record[20:28])
        if offset + payload_size > table_start:
            raise ValueError(f"FARC payload {index} extends into table: {path}")
        entries.append((record[:20], offset, payload_size))
    return entries


def merge_ranges(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for start, end in sorted(ranges):
        if not merged or start > merged[-1][1]:
            merged.append([start, end])
        else:
            merged[-1][1] = max(merged[-1][1], end)
    return [(start, end) for start, end in merged]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("filedb", type=Path)
    parser.add_argument("trace", type=Path)
    parser.add_argument("archives", nargs="+", type=Path)
    parser.add_argument("--limit", type=int, default=30, help="maximum asset names to print per archive")
    args = parser.parse_args()

    paths = parse_filedb(args.filedb)
    requested: dict[str, list[tuple[int, int]]] = defaultdict(list)
    for line in args.trace.read_text(encoding="utf-8", errors="replace").splitlines():
        match = TRACE.search(line)
        if not match:
            continue
        operation, filename, requested_size, offset, result = match.groups()
        if operation != "pread" or int(result) <= 0:
            continue
        name = Path(filename).name.lower()
        if name in FARC_NAMES:
            start, end = int(offset), int(offset) + int(result)
            if start >= 0 and end > start:
                requested[name].append((start, end))

    total_errors = 0
    for archive_path in args.archives:
        name = archive_path.name.lower()
        ranges = merge_ranges(requested.get(name, []))
        covered_bytes = sum(end - start for start, end in ranges)
        touched: list[tuple[int, int, str, bool]] = []
        for digest, offset, size in parse_farc(archive_path):
            if not size:
                continue
            end = offset + size
            overlap = sum(max(0, min(end, r_end) - max(offset, r_start)) for r_start, r_end in ranges)
            if overlap:
                logical_names = paths.get((digest, size), [])
                touched.append((offset, overlap, logical_names[0] if logical_names else "<no FileDB path>", overlap == size))
        print(
            f"{archive_path.name}: traced_bytes={covered_bytes} read_ranges={len(ranges)} "
            f"payloads_touched={len(touched)} payloads_fully_covered={sum(row[3] for row in touched)}"
        )
        total_errors += sum(row[2] == "<no FileDB path>" for row in touched)
        for offset, overlap, logical_name, full in sorted(touched)[: max(0, args.limit)]:
            print(f"  0x{offset:x} {'full' if full else f'{overlap} bytes'} {logical_name}")
        if len(touched) > args.limit:
            print(f"  ... {len(touched) - args.limit} more assets omitted")
    return 1 if total_errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
