#!/usr/bin/env python3
"""Read-only structural audit for LBP3 FARC archive footer tables.

The tool reads only each 8-byte footer and its entry table. It does not extract
or modify archive contents.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

MAGIC = b"FARC"
ENTRY_SIZE = 28  # SHA-1 (20), absolute data offset (u32 BE), size (u32 BE)
EXPECTED_NAMES = {"intro_001.farc", "base_001.farc", "craftworld_001.farc", "chunk1_001.farc"}


def audit(path: Path) -> bool:
    size = path.stat().st_size
    if size < 8:
        print(f"FAIL {path}: shorter than FARC footer")
        return False

    with path.open("rb") as archive:
        archive.seek(size - 8)
        footer = archive.read(8)
        count_bytes, magic = footer[:4], footer[4:]
        if magic != MAGIC:
            print(f"FAIL {path}: footer magic is {magic!r}, expected {MAGIC!r}")
            return False

        count = int.from_bytes(count_bytes, "big")
        table_bytes = count * ENTRY_SIZE
        table_start = size - 8 - table_bytes
        if count == 0 or table_start < 0:
            print(f"FAIL {path}: invalid entry count {count}")
            return False

        archive.seek(table_start)
        table = archive.read(table_bytes)
        if len(table) != table_bytes:
            print(f"FAIL {path}: short entry table ({len(table)}/{table_bytes})")
            return False

        invalid_bounds = 0
        sha1_mismatches = 0
        first_offset = None
        last_offset = None
        for index in range(count):
            entry = table[index * ENTRY_SIZE : (index + 1) * ENTRY_SIZE]
            expected_sha1 = entry[:20]
            data_offset, data_size = struct.unpack(">II", entry[20:28])
            if index == 0:
                first_offset = data_offset
            if index == count - 1:
                last_offset = data_offset
            if data_offset + data_size > table_start:
                invalid_bounds += 1
                continue

            archive.seek(data_offset)
            digest = hashlib.sha1(archive.read(data_size)).digest()
            if digest != expected_sha1:
                sha1_mismatches += 1

    passed = invalid_bounds == 0 and sha1_mismatches == 0
    status = "PASS" if passed else "FAIL"
    print(
        f"{status} {path.name}: bytes={size} entries={count} "
        f"table=0x{table_start:x}+0x{table_bytes:x} "
        f"data_bounds_errors={invalid_bounds} sha1_mismatches={sha1_mismatches} "
        f"first_entry_offset=0x{first_offset:x} last_entry_offset=0x{last_offset:x}"
    )
    return passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path, help="FARC files or directories containing them")
    args = parser.parse_args()

    paths: list[Path] = []
    for target in args.paths:
        if target.is_dir():
            paths.extend(path for path in target.rglob("*.farc") if path.name.lower() in EXPECTED_NAMES)
        else:
            paths.append(target)
    paths = sorted(set(paths))
    if not paths:
        print("No FARC files found", file=sys.stderr)
        return 2

    results = [audit(path) for path in paths]
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
