#!/usr/bin/env python3
"""Cross-check an LBP3 FileDB/GUID map against supplied FARC archives.

The binary layout follows FeTetra/cwlib-rs src/io/filedb.rs and farc.rs.
This is read-only and reports which map payloads are present in the archives.
"""

from __future__ import annotations

import argparse
import struct
from collections import Counter
from pathlib import Path

FARC_MAGIC = b"FARC"
LBP3_REVISION = 0x01480100
ENTRY_SIZE = 28


def load_farc(path: Path) -> Counter[tuple[bytes, int]]:
    size = path.stat().st_size
    with path.open("rb") as archive:
        archive.seek(size - 8)
        footer = archive.read(8)
        count, magic = footer[:4], footer[4:]
        if magic != FARC_MAGIC:
            raise ValueError(f"{path}: bad FARC magic {magic!r}")
        entry_count = int.from_bytes(count, "big")
        table_size = entry_count * ENTRY_SIZE
        table_start = size - 8 - table_size
        if table_start < 0:
            raise ValueError(f"{path}: impossible table size")
        archive.seek(table_start)
        table = archive.read(table_size)
        if len(table) != table_size:
            raise ValueError(f"{path}: short table")
        entries: Counter[tuple[bytes, int]] = Counter()
        for index in range(entry_count):
            entry = table[index * ENTRY_SIZE : (index + 1) * ENTRY_SIZE]
            digest = entry[:20]
            offset, payload_size = struct.unpack(">II", entry[20:28])
            if offset + payload_size > table_start:
                raise ValueError(f"{path}: entry {index} extends into footer table")
            entries[(digest, payload_size)] += 1
        return entries


def audit(map_path: Path, archives: list[Path]) -> int:
    available: Counter[tuple[bytes, int]] = Counter()
    for archive in archives:
        entries = load_farc(archive)
        available.update(entries)
        print(f"FARC {archive.name}: {sum(entries.values())} indexed payloads")

    data = map_path.read_bytes()
    if len(data) < 8:
        raise ValueError(f"{map_path}: truncated FileDB header")
    revision, count = struct.unpack(">II", data[:8])
    if revision != LBP3_REVISION:
        raise ValueError(f"{map_path}: revision 0x{revision:08x} is not LBP3")

    cursor = 8
    parsed = empty = matched = absent = 0
    malformed: str | None = None
    examples: list[str] = []
    for index in range(count):
        if cursor + 2 > len(data):
            malformed = f"entry {index}: missing path length at 0x{cursor:x}"
            break
        path_size = int.from_bytes(data[cursor : cursor + 2], "big")
        cursor += 2
        end_path = cursor + path_size
        end_record = end_path + 32  # date, size, SHA-1, GUID
        if end_record > len(data):
            malformed = f"entry {index}: record runs past EOF"
            break
        raw_path = data[cursor:end_path]
        record = data[end_path:end_record]
        cursor = end_record
        parsed += 1
        if not path_size:
            empty += 1
            continue
        try:
            name = raw_path.decode("utf-8")
        except UnicodeDecodeError:
            malformed = f"entry {index}: invalid UTF-8 path"
            break
        payload_size = int.from_bytes(record[4:8], "big")
        digest = record[8:28]
        if available[(digest, payload_size)]:
            matched += 1
            available[(digest, payload_size)] -= 1
        else:
            absent += 1
            if len(examples) < 12:
                examples.append(name)

    print(
        f"MAP {map_path.name}: revision=0x{revision:08x} declared={count} "
        f"parsed={parsed} empty={empty} matched={matched} absent_from_supplied_FARCs={absent} "
        f"cursor=0x{cursor:x}/0x{len(data):x}"
    )
    if malformed:
        print(f"MAP ERROR: {malformed}")
        return 1
    for name in examples:
        print(f"  absent example: {name}")
    if parsed != count:
        print(f"MAP ERROR: parsed {parsed} of {count} entries")
        return 1
    if cursor != len(data):
        print(f"MAP NOTE: {len(data) - cursor} trailing bytes after declared entries")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map", type=Path, help="LBP3 orbisguids.map")
    parser.add_argument("archives", nargs="+", type=Path, help="FARC archives to cross-check")
    args = parser.parse_args()
    return audit(args.map, args.archives)


if __name__ == "__main__":
    raise SystemExit(main())
