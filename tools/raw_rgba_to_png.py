#!/usr/bin/env python3
"""Convert a tightly packed RGBA8 frame dump to PNG using only the stdlib."""
import argparse
import struct
import zlib
from pathlib import Path


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    args = parser.parse_args()
    pixels = args.input.read_bytes()
    expected = args.width * args.height * 4
    if len(pixels) != expected:
        raise SystemExit(f"expected {expected} RGBA bytes, got {len(pixels)}")
    stride = args.width * 4
    scanlines = b"".join(b"\0" + pixels[y * stride:(y + 1) * stride] for y in range(args.height))
    header = struct.pack(">IIBBBBB", args.width, args.height, 8, 6, 0, 0, 0)
    args.output.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                            chunk(b"IDAT", zlib.compress(scanlines, 6)) + chunk(b"IEND", b""))
    print(f"wrote {args.width}x{args.height} PNG: {args.output}")


if __name__ == "__main__":
    main()
