"""Convert a tightly packed 32-bit RGBA frame dump to a BMP for inspection."""
import argparse
from pathlib import Path
import struct

parser = argparse.ArgumentParser()
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("width", type=int)
parser.add_argument("height", type=int)
args = parser.parse_args()
pixels = args.input.read_bytes()
stride = args.width * 4
if len(pixels) != stride * args.height:
    raise SystemExit(f"Expected {stride * args.height} bytes, found {len(pixels)}")
# BMP stores rows bottom-up and pixels as BGRA; swap the red and blue channels.
rows = bytearray()
for y in range(args.height - 1, -1, -1):
    row = pixels[y * stride : (y + 1) * stride]
    for x in range(0, stride, 4):
        rows.extend((row[x + 2], row[x + 1], row[x], row[x + 3]))
header = struct.pack("<2sIHHI", b"BM", 54 + len(rows), 0, 0, 54)
header += struct.pack("<IiiHHIIiiII", 40, args.width, args.height, 1, 32, 0, len(rows), 2835, 2835, 0, 0)
args.output.write_bytes(header + rows)
