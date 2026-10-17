import importlib.util
from pathlib import Path
import struct
import unittest

root=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result
tool=module('audit_libc_init',root/'tools/audit_libc_init.py')
fixtures=module('imports_fixture',Path(__file__).with_name('imports_test.py'))

def fixture(init=None):
    data=fixtures.fixture()
    struct.pack_into('<H',data,56,3)
    struct.pack_into('<II6Q',data,176,1,5,0,0,0,len(data),len(data),1)
    if init is not None:
        struct.pack_into('<QQ',data,400,12,init)
        struct.pack_into('<QQ',data,416,0,0)
        struct.pack_into('<Q',data,64+32,176)
    return data

class Audit(unittest.TestCase):
    def test_heap_table_relocations_and_optional_nulls(self):
        data=fixture(0)
        struct.pack_into('<QQq',data,1280,600,8,64)
        table=tool.audit(data,heap_table_va=600)['heap_api_table']
        self.assertEqual(len(table),10)
        self.assertEqual(table[0]['relative_guest_va'],'0x40')
        self.assertTrue(all(slot['raw']=='0x0' and slot['relocation'] is None for slot in table[1:]))
    def test_truncated_heap_table_rejected(self):
        with self.assertRaisesRegex(ValueError,'outside PT_LOAD'):
            tool.audit(fixture(0),heap_table_va=2024)
    def test_reverse_constructors_and_zero_skip(self):
        def pointer(slot):
            raw, relocation = {24:(0,None),16:(0,dict(type=8,addend=64)),
                               8:(0xffffffffffffffff,None)}[slot]
            return dict(slot=hex(slot),raw=hex(raw),relocation=relocation)
        result=tool.legacy_constructors(24,pointer,lambda va:va==64)
        self.assertEqual(result['call_order'],['0x40'])
        self.assertEqual(result['sentinel_slot'],'0x8')
        with self.assertRaisesRegex(ValueError,'outside executable'):
            tool.legacy_constructors(24,pointer,lambda va:False)
    def test_constructor_scan_is_bounded(self):
        def zero(slot):return dict(slot=hex(slot),raw='0x0',relocation=None)
        with self.assertRaisesRegex(ValueError,'Missing bounded'):
            tool.legacy_constructors(32,zero,lambda va:True,max_slots=2)
        with self.assertRaisesRegex(ValueError,'Unaligned'):
            tool.legacy_constructors(3,zero,lambda va:True)
    def test_unresolved_constructor_rejected(self):
        def external(slot):return dict(slot=hex(slot),raw='0x0',relocation=dict(type=1,addend=64))
        with self.assertRaisesRegex(ValueError,'not a relative'):
            tool.legacy_constructors(8,external,lambda va:True)
    def test_zero_init_is_present(self):
        result=tool.audit(fixture(0))
        self.assertEqual(result['lifecycle']['init'],dict(present=True,guest_va='0x0'))
    def test_absent_init_is_distinct(self):
        self.assertEqual(tool.audit(fixture())['lifecycle']['init'],dict(present=False,guest_va=None))
    def test_init_outside_executable_data_rejected(self):
        with self.assertRaisesRegex(ValueError,'outside file-backed executable'):
            tool.audit(fixture(4096))

if __name__=='__main__': unittest.main()
