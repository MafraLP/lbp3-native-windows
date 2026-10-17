"""Run native linker against synthetic modules and inspect verified memory values."""
from pathlib import Path
import re
import csv
import struct
import subprocess
import sys
import tempfile
import unittest

EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())


def image(provider=False, version=1, target=0x2000, overlap=False, bad_symbol=False, tls=False):
    data = bytearray(4096)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHI', data, 16, 0xfe18, 62, 1)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<HHH', data, 52, 64, 56, 3)
    strings = b'\0libTest\0modTest\0Test#B#C\0'
    rels = [(0x2020, (1 << 32) | 1, 4)] if provider else [
        (target, 8, 0x100), (0x2008, (1 << 32) | 1, -8),
        (0x2010 if not overlap else target+4, (1 << 32) | 6, 0),
        (0x2018, (1 << 32) | (16 if tls else 7), 0)]
    if bad_symbol:
        rels[1] = (0x2008, (99 << 32) | 1, 0)
    tags = [(0x61000035, 0), (0x61000037, len(strings)),
            (0x61000039, 0x100), (0x6100003f, 48), (0x6100003b, 24),
            (0x61000013 if provider else 0x61000015, (1 << 48) | (version << 32) | 1),
            (0x6100000d if provider else 0x6100000f, (2 << 48) | 9),
            (0x6100002f, 0x200), (0x61000031, 24*len(rels)), (0x61000033, 24), (0, 0)]
    struct.pack_into('<II6Q', data, 64, 1, 6, 0, 0, 0, 4096, 0x4000, 4096)
    struct.pack_into('<II6Q', data, 120, 2, 0, 0x300, 0, 0, len(tags)*16, 0, 8)
    struct.pack_into('<II6Q', data, 176, 0x61000000, 0, 0x500, 0, 0, 0x400, 0, 8)
    for i, pair in enumerate(tags):
        struct.pack_into('<qQ', data, 0x300+16*i, *pair)
    data[0x500:0x500+len(strings)] = strings
    struct.pack_into('<IBBHQQ', data, 0x600+24, 17, 0x12, 0, 1 if provider else 0, 0x1000 if provider else 0, 1)
    for i, rel in enumerate(rels):
        struct.pack_into('<QQq', data, 0x700+24*i, *rel)
    return data


def host_image(version=1, nid=b'1jfXLRVzisc', kind=2):
    data = image()
    strings = b'\0libkernel\0libkernel\0' + nid + b'#B#C\0'
    data[0x500:0x500+len(strings)] = strings
    struct.pack_into('<Q', data, 0x318, len(strings))
    struct.pack_into('<Q', data, 0x358, (1 << 48) | (version << 32) | 1)
    struct.pack_into('<Q', data, 0x368, (2 << 48) | (0x0101 << 32) | 11)
    struct.pack_into('<I', data, 0x618, 21)
    struct.pack_into('<B', data, 0x61c, 0x10 | kind)
    return data


def tls_image(bad_backing=False, unsupported=False):
    data = image(tls=True)
    struct.pack_into('<H', data, 56, 4)
    struct.pack_into('<II6Q', data, 232, 7, 4, 0x901 if bad_backing else 0x900, 0x900, 0, 2, 64, 16)
    data[0x900:0x902] = bytes([0x42, 0x17])
    struct.pack_into('<Q', data, 0x338, 72)  # DT_SCE_SYMTABSZ
    struct.pack_into('<IBBHQQ', data, 0x630, 0, 3, 0, 0, 0, 0)
    struct.pack_into('<Q', data, 0x750, (2 << 32) | (17 if unsupported else 16))
    return data


def process_image(valid=True):
    data=image()
    struct.pack_into('<H',data,16,0xfe10)
    struct.pack_into('<I',data,68,5)
    struct.pack_into('<H',data,56,4)
    struct.pack_into('<II6Q',data,232,0x61000001,4,0x900,0x900,0,64,64,8)
    struct.pack_into('<QII',data,0x900,64,0x4942524f if valid else 0,1)
    return data


class LinkSet(unittest.TestCase):
    def test_explicit_internal_libc_finalizer_bridge(self):
        def module(provider, library, version=1, kind=2, nid='H2e8t5ScQGc'):
            data=image(provider)
            name=library.encode()
            strings=b'\0'+name+b'\0'+name+b'\0'+nid.encode()+b'#B#C\0'
            data[0x500:0x500+len(strings)]=strings
            struct.pack_into('<Q',data,0x318,len(strings))
            struct.pack_into('<Q',data,0x358,(1<<48)|(version<<32)|1)
            struct.pack_into('<Q',data,0x368,(2<<48)|(0x0101<<32)|(len(name)+2))
            struct.pack_into('<I',data,0x618,len(name)*2+3)
            struct.pack_into('<B',data,0x61c,0x10|kind)
            return data
        provider=module(True,'libc')
        result=self.run_set(module(False,'libSceLibcInternal'),provider)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('guest_bindings=3 pending=0',result.stdout)
        self.assertIn('compatibility_bindings=3',result.stdout)
        for library,version,kind,nid in [('wrong',1,2,'H2e8t5ScQGc'),('libSceLibcInternal',2,2,'H2e8t5ScQGc'),
                                         ('libSceLibcInternal',1,1,'H2e8t5ScQGc'),('libSceLibcInternal',1,2,'UnknownNID')]:
            rejected=self.run_set(module(False,library,version,kind,nid),provider)
            self.assertEqual(rejected.returncode,0,rejected.stderr)
            self.assertIn('pending=3',rejected.stdout)
            self.assertIn('compatibility_bindings=0',rejected.stdout)
    def test_explicit_internal_libc_memcpy_bridge(self):
        def module(provider,library,nid='Q3VBxCXhUHs',version=1,kind=2):
            data=image(provider)
            name=library.encode()
            strings=b'\0'+name+b'\0'+name+b'\0'+nid.encode()+b'#B#C\0'
            data[0x500:0x500+len(strings)]=strings
            struct.pack_into('<Q',data,0x318,len(strings))
            struct.pack_into('<Q',data,0x358,(1<<48)|(version<<32)|1)
            struct.pack_into('<Q',data,0x368,(2<<48)|(0x0101<<32)|(len(name)+2))
            struct.pack_into('<I',data,0x618,len(name)*2+3)
            struct.pack_into('<B',data,0x61c,0x10|kind)
            return data
        provider=module(True,'libc')
        result=self.run_set(module(False,'libSceLibcInternal'),provider)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('guest_bindings=3 pending=0',result.stdout)
        self.assertIn('compatibility_bindings=3',result.stdout)
        for library,nid,version,kind in [('libSceLibcInternal','UnknownNID',1,2),
                                          ('wrong','Q3VBxCXhUHs',1,2),
                                          ('libSceLibcInternal','Q3VBxCXhUHs',2,2),
                                          ('libSceLibcInternal','Q3VBxCXhUHs',1,1)]:
            rejected=self.run_set(module(False,library,nid,version,kind),provider)
            self.assertEqual(rejected.returncode,0,rejected.stderr)
            self.assertIn('pending=3',rejected.stdout)
            self.assertIn('compatibility_bindings=0',rejected.stdout)
    def test_explicit_internal_libc_memset_bridge(self):
        def module(provider,library,nid='8zTFvBIAIN8'):
            data=image(provider)
            name=library.encode()
            strings=b'\0'+name+b'\0'+name+b'\0'+nid.encode()+b'#B#C\0'
            data[0x500:0x500+len(strings)]=strings
            struct.pack_into('<Q',data,0x318,len(strings))
            struct.pack_into('<Q',data,0x358,(1<<48)|(1<<32)|1)
            struct.pack_into('<Q',data,0x368,(2<<48)|(0x0101<<32)|(len(name)+2))
            struct.pack_into('<I',data,0x618,len(name)*2+3)
            struct.pack_into('<B',data,0x61c,0x12)
            return data
        provider=module(True,'libc')
        result=self.run_set(module(False,'libSceLibcInternal'),provider)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('guest_bindings=3 pending=0',result.stdout)
        self.assertIn('compatibility_bindings=3',result.stdout)
        rejected=self.run_set(module(False,'libSceLibcInternal','UnknownNID'),provider)
        self.assertEqual(rejected.returncode,0,rejected.stderr)
        self.assertIn('pending=3',rejected.stdout)
        self.assertIn('compatibility_bindings=0',rejected.stdout)
    def test_undefined_weak_zero_after_lookup(self):
        data=image(version=2)
        struct.pack_into('<B',data,0x61c,0x22)
        struct.pack_into('<q',data,0x700+24+16,4)
        result=self.run_set(data)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('pending=0 host_bindings=0 tls_relocations=0 weak_zero=3',result.stdout)
        self.assertRegex(result.stdout,r'target=0x2008 value=0x4\b')
        self.assertRegex(result.stdout,r'target=0x2010 value=0x0\b')
        struct.pack_into('<B',data,0x61c,0x12)
        strong=self.run_set(data)
        self.assertIn('pending=3',strong.stdout)
    def test_weak_prefers_real_export(self):
        data=image()
        struct.pack_into('<B',data,0x61c,0x22)
        result=self.run_set(data)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('guest_bindings=3 pending=0',result.stdout)
        self.assertIn('weak_zero=0',result.stdout)
    def run_set(self, request=None, provider=None):
        with tempfile.TemporaryDirectory(prefix='lbp3-link-') as temp:
            directory = Path(temp)
            (directory/'provider.elf').write_bytes(image(True) if provider is None else provider)
            (directory/'request.elf').write_bytes(image() if request is None else request)
            pending = directory/'pending.csv'
            result = subprocess.run([EXECUTABLE, '--link-set', temp, str(pending)], capture_output=True, text=True, timeout=15)
            result.pending_rows = []
            if pending.exists():
                with pending.open(encoding='utf-8', newline='') as f:
                    result.pending_rows = list(csv.DictReader(f))
            return result

    def test_real_writes_match_independent_address_calculation(self):
        result = self.run_set()
        self.assertEqual(result.returncode, 0, result.stderr)
        biases = dict((name, int(value, 16)) for name, value in re.findall(r'Module: (\S+)\nLoad bias: 0x([0-9a-f]+)', result.stdout))
        values = {(name, int(target,16)): int(value,16) for name, target, value in re.findall(
            r'Verified relocation \[(\S+)\]: target=0x([0-9a-f]+) value=0x([0-9a-f]+)', result.stdout)}
        self.assertEqual(values['request.elf',0x2000], biases['request.elf']+0x100)
        self.assertEqual(values['request.elf',0x2008], biases['provider.elf']+0x1000-8)
        self.assertEqual(values['request.elf',0x2010], biases['provider.elf']+0x1000)
        self.assertEqual(values['request.elf',0x2018], biases['provider.elf']+0x1000)
        self.assertEqual(values['provider.elf',0x2020], biases['provider.elf']+0x1000+4)
        self.assertIn('relative=1 local=1 guest_bindings=3 pending=0', result.stdout)

    def test_relative_ignores_nonzero_symbol(self):
        request=image()
        struct.pack_into('<Q', request, 0x708, (1 << 32) | 8)
        result=self.run_set(request=request)
        self.assertEqual(result.returncode, 0, result.stderr)
        biases=dict((name,int(value,16)) for name,value in re.findall(r'Module: (\S+)\nLoad bias: 0x([0-9a-f]+)',result.stdout))
        value=re.search(r'Verified relocation \[request.elf\]: target=0x2000 value=0x([0-9a-f]+)',result.stdout)
        self.assertIsNotNone(value)
        self.assertEqual(int(value[1],16),biases['request.elf']+0x100)
        self.assertIn('pending=0',result.stdout)

    def test_version_mismatch_stays_pending(self):
        result = self.run_set(provider=image(True, version=2))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('guest_bindings=0 pending=3', result.stdout)
        self.assertEqual(len(result.pending_rows), 3)
        self.assertTrue(all(row['reason'] == 'unresolved_symbol' for row in result.pending_rows))

    def test_tls_stays_pending(self):
        result = self.run_set(request=image(tls=True))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('guest_bindings=2 pending=1', result.stdout)
        self.assertEqual(result.pending_rows[0]['reason'], 'tls_relocation')

    def test_orbis_local_tls_module_id_is_written(self):
        result = self.run_set(request=tls_image())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('pending=0 host_bindings=0 tls_relocations=1', result.stdout)
        self.assertRegex(result.stdout, r'Verified relocation \[request.elf\]: target=0x2018 value=0x1\b')
        self.assertIn('Thread TLS prepared: 1 modules, 64 bytes.', result.stdout)

    def test_other_tls_types_stay_pending(self):
        result = self.run_set(request=tls_image(unsupported=True))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('pending=1 host_bindings=0 tls_relocations=0', result.stdout)

    def test_tls_initializer_must_match_load_backing(self):
        result = self.run_set(request=tls_image(bad_backing=True))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('TLS initializer is not backed', result.stderr)
        self.assertNotIn('Verified relocation', result.stdout)

    def test_typed_host_import_is_bound(self):
        result = self.run_set(request=host_image())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('guest_bindings=0 pending=0 host_bindings=3', result.stdout)
        self.assertFalse(result.pending_rows)

    def test_tls_get_addr_host_import_is_bound(self):
        result = self.run_set(request=host_image(nid=b'vNe1w4diLCs'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('guest_bindings=0 pending=0 host_bindings=3', result.stdout)
        self.assertFalse(result.pending_rows)

    def test_stack_guard_is_a_typed_data_export(self):
        result=self.run_set(request=host_image(nid=b'f7uOxY9mM1U',kind=1))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('pending=0 host_bindings=3',result.stdout)
        wrong=self.run_set(request=host_image(nid=b'f7uOxY9mM1U',kind=2))
        self.assertEqual(wrong.returncode,0,wrong.stderr)
        self.assertIn('pending=3 host_bindings=0',wrong.stdout)

    def test_pthread_api_nids_are_bound(self):
        for nid in (b'6UgtwV+0zb4', b'onNY9Byn-W8'):
            result = self.run_set(request=host_image(nid=nid))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('guest_bindings=0 pending=0 host_bindings=3', result.stdout)
            self.assertFalse(result.pending_rows)

    def test_process_parameters_loaded_from_main_elf(self):
        result=self.run_set(request=process_image())
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('Process parameters verified [request.elf]: guest_va=0x900 bytes=64',result.stdout)
        invalid=self.run_set(request=process_image(False))
        self.assertNotEqual(invalid.returncode,0)
        self.assertIn('process parameter size, magic or version',invalid.stderr)

    def test_process_and_mutex_host_nids_are_bound(self):
        for nid in (b'959qrazPIrg',b'F8bUHwAG284',b'cmo1RIYva9o',b'9UK1vLZQft4',b'tn3VlD0hG60',b'p5EcQeEeJAE',
                    b'mL8NDH86iQI',b'cQke9UuBQOk',b'3k6kx-zOOSQ',b'9BcDykPmo1I',b'k+AXqu2-eBc',
                    b'14bOACANTBo',b'geDaqgH9lTg',b'PrdHuuDekhY',b'+BzXYkqYeLE',b'eoht7mQOCmo',
                    b'BPE9s9vQQXo',b'UqDGjXA5yUM',b'2Tb92quprl0',b'g+PZd2hiacg',b'WKAXJ4XBPQ4',
                    b'kDh-NfxgMtE',b'JGgj7Uvrl+A',b'OMDRKKAZ8I4',b'zE-wXIZjLoM',b'6Z83sYWFlA8',
                    b'VkTAsrZDcJ0',b'aPcyptbOiZs',b'f7KBOafysXo',b'Fjc4-n1+y2g',b'crb5j7mkk1c',
                    b'1G3lF1Gg1k8',b'UK2Tl2DWUns',b'Cg4srZ6TKbU',b'4wSze92BhLI',b'+r3rMFwItV4',b'nKWi-N2HBV4',
                    b'oib76F-12fk',b'kBwCPsYX-m4',b'eV9wAD2riIA',b'fTx66l5iWIA',b'VW3TVZiM4-E',b'WlyEA-sLDf0',
                    b'1-LFLmRFxxM',b'naInUjYt3so',b'AUXVxWeJU-A',b'52NcYU9+lEo',b'mqQMh1zPPT8',b'BmMjYxmew1w',
                    b'aI+OeCz8xrQ',b'3PtV6p3QNX4',b'T72hz6ffq08'):
            result=self.run_set(request=host_image(nid=nid))
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('pending=0 host_bindings=3',result.stdout)

    def test_wrong_host_version_stays_pending(self):
        result = self.run_set(request=host_image(version=2))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('pending=3 host_bindings=0', result.stdout)
        self.assertEqual(len(result.pending_rows), 3)

    def test_invalid_target_fails_before_writes(self):
        result = self.run_set(request=image(target=0x4000))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('outside PT_LOAD', result.stderr)
        self.assertNotIn('Verified relocation', result.stdout)

    def test_overlap_fails_before_writes(self):
        result = self.run_set(request=image(overlap=True))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Overlapping relocation', result.stderr)
        self.assertNotIn('Verified relocation', result.stdout)

    def test_invalid_symbol_index_fails(self):
        result = self.run_set(request=image(bad_symbol=True))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Invalid relocation symbol index', result.stderr)


if __name__ == '__main__':
    unittest.main()
