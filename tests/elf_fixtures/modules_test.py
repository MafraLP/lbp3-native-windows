import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('prepare_modules', Path(__file__).resolve().parents[2] / 'tools/prepare_modules.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def fixture():
    data = bytearray(544)
    data[:4] = bytes.fromhex('4f153d1d')
    struct.pack_into('<H', data, 24, 2)
    # Entries deliberately in reverse program-header order, equal payload sizes.
    struct.pack_into('<4Q', data, 32, (1 << 20) | 0x800, 512, 16, 16)
    struct.pack_into('<4Q', data, 64, 0x800, 528, 16, 16)
    data[96:103] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<H', data, 96+18, 62)
    struct.pack_into('<Q', data, 96+32, 64)
    struct.pack_into('<HH', data, 96+54, 56, 2)
    struct.pack_into('<II6Q', data, 160, 1, 5, 256, 0, 0, 16, 16, 1)
    struct.pack_into('<II6Q', data, 216, 1, 6, 272, 4096, 0, 16, 32, 1)
    data[512:528] = b'B'*16
    data[528:544] = b'A'*16
    return data


class Modules(unittest.TestCase):
    def test_segment_index_not_size(self):
        elf, report = tool.convert_self(fixture())
        self.assertEqual(elf[256:272], b'A'*16)
        self.assertEqual(elf[272:288], b'B'*16)
        self.assertEqual(len(report['copied_segments']), 2)

    def test_reject_encrypted_and_compressed(self):
        for flag in (2, 8):
            data = fixture()
            struct.pack_into('<Q', data, 32, (1 << 20) | 0x800 | flag)
            with self.assertRaises(ValueError):
                tool.convert_self(data)

    def test_missing_source_payload(self):
        data = fixture()
        struct.pack_into('<Q', data, 32, 0)
        with self.assertRaisesRegex(ValueError, 'missing source bytes'):
            tool.convert_self(data)

    def test_truncated_payload(self):
        with self.assertRaises(ValueError):
            tool.convert_self(fixture()[:-1])

    def test_invalid_index(self):
        data = fixture()
        struct.pack_into('<Q', data, 32, (9 << 20) | 0x800)
        with self.assertRaisesRegex(ValueError, 'segment index'):
            tool.convert_self(data)

    def test_exact_identity_and_ambiguous_exports(self):
        symbol = dict(symbol_index=1, nid='test', library='libTest', module='moduleTest',
                      library_version=1, module_version=1, kind='function',
                      binding='global', section=1, value=0)
        request = dict(symbol, section=0)
        requester = dict(name='main', inventory=dict(imports=[request], exports=[]))
        provider = dict(name='provider', inventory=dict(imports=[], exports=[symbol]))
        self.assertEqual(tool.bindings([requester, provider])[0]['status'], 'guest_candidate')
        symbol['library_version'] = 2
        self.assertEqual(tool.bindings([requester, provider])[0]['status'], 'external_unresolved')
        symbol['library_version'] = 1
        duplicate = dict(name='duplicate', inventory=provider['inventory'])
        self.assertEqual(tool.bindings([requester, provider, duplicate])[0]['status'], 'ambiguous')


if __name__ == '__main__':
    unittest.main()
