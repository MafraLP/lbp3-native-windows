"""Synthetic tests; no game files required."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('inspect_imports', Path(__file__).resolve().parents[2] / 'tools/inspect_imports.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def fixture():
    data = bytearray(2048)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<H', data, 18, 62)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<HH', data, 54, 56, 2)
    strings = b'\0libTest\0moduleTest\0TestNID#B#C\0'
    tags = [(0x61000035, 0), (0x61000037, len(strings)),
            (0x61000039, 128), (0x6100003f, 72), (0x6100003b, 24),
            (0x61000015, (1 << 48) | 1), (0x6100000f, (2 << 48) | 9),
            (0x61000029, 256), (0x6100002d, 24), (0, 0)]
    struct.pack_into('<II6Q', data, 64, 2, 0, 256, 0, 0, len(tags)*16, 0, 8)
    struct.pack_into('<II6Q', data, 120, 0x61000000, 0, 1024, 0, 0, 512, 0, 8)
    for i, pair in enumerate(tags):
        struct.pack_into('<qQ', data, 256+i*16, *pair)
    data[1024:1024+len(strings)] = strings
    struct.pack_into('<IBBHQQ', data, 1024+128+24, 20, 0x12, 0, 0, 0, 0)
    # A defined symbol must not be counted as an import.
    struct.pack_into('<IBBHQQ', data, 1024+128+48, 20, 0x12, 0, 1, 1, 0)
    struct.pack_into('<QQq', data, 1280, 4096, (1 << 32) | 7, 0)
    return data


class Imports(unittest.TestCase):
    def test_scope_and_associations(self):
        result = tool.inspect(fixture())
        self.assertEqual(len(result['imports']), 1)
        row = result['imports'][0]
        self.assertEqual((row['library'], row['module'], row['nid']), ('libTest', 'moduleTest', 'TestNID'))
        self.assertEqual(row['relocation_count'], 1)

    def test_truncated_segment(self):
        with self.assertRaises(ValueError):
            tool.inspect(fixture()[:1200])

    def test_bad_string_offset(self):
        data = fixture()
        struct.pack_into('<I', data, 1176, 999)
        with self.assertRaises(ValueError):
            tool.inspect(data)

    def test_bad_relocation_symbol(self):
        data = fixture()
        struct.pack_into('<Q', data, 1288, (999 << 32) | 7)
        with self.assertRaises(ValueError):
            tool.inspect(data)


if __name__ == '__main__':
    unittest.main()
