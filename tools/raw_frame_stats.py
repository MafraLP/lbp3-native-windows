"""Summarize pixels in a tightly packed 32-bit RGBA frame dump."""
import argparse
from collections import Counter
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("input", type=Path)
parser.add_argument("width", type=int)
parser.add_argument("height", type=int)
args = parser.parse_args()
data = args.input.read_bytes()
expected = args.width * args.height * 4
if len(data) != expected:
    raise SystemExit(f"Expected {expected} bytes, found {len(data)}")
pixels = [tuple(data[i : i + 4]) for i in range(0, len(data), 4)]
print(f"pixels={len(pixels)} unique={len(set(pixels))} nonzero={sum(any(p[:3]) for p in pixels)}")
print("channel min/max:", [(min(p[c] for p in pixels), max(p[c] for p in pixels)) for c in range(4)])
print("common RGBA:", Counter(pixels).most_common(12))
