import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('gate',Path(__file__).resolve().parents[2]/'tools/check_libc_static_gate.py')
tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)


class Gate(unittest.TestCase):
    def fixture(self):
        audit=dict(sha256='hash',lifecycle=dict(init=dict(guest_va='0x0'),preinit_array=dict(slots=[dict(relative_guest_va='0x10')])),
                   legacy_constructors=dict(call_order=['0x20']))
        graph=dict(elf_sha256='hash',roots=['0x0','0x10','0x20'],reachable_pending_imports=[],barriers=[])
        return audit,graph
    def test_zero_init_and_complete_graph(self):
        audit,graph=self.fixture();self.assertFalse(tool.check(audit,graph,'hash'))
    def test_pending_import_and_barrier_block(self):
        audit,graph=self.fixture();graph['reachable_pending_imports']=[{}];graph['barriers']=[{}]
        self.assertEqual(len(tool.check(audit,graph,'hash')),2)
    def test_mismatched_hash_rejected(self):
        audit,graph=self.fixture()
        with self.assertRaisesRegex(ValueError,'hashes differ'):tool.check(audit,graph,'other')
    def test_missing_root_rejected(self):
        audit,graph=self.fixture();graph['roots'].remove('0x0')
        with self.assertRaisesRegex(ValueError,'missing reviewed'):tool.check(audit,graph,'hash')


if __name__=='__main__':unittest.main()
