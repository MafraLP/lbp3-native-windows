import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('trace',Path(__file__).resolve().parents[2]/'tools/trace_libc_startup.py')
tool=importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class Trace(unittest.TestCase):
    def test_noreturn_call_does_not_walk_padding(self):
        code={0:(b'xxxxx','callq','0x10'),5:(b'x','<unknown>','')}
        result=tool.trace(code,[0],{16:'exit'},nonreturning={16})
        self.assertFalse(result['barriers'])
        self.assertEqual(result['visited_instructions'],2)
    def test_parser_and_call_fallthrough(self):
        code=tool.instructions(' 0: e8 0b 00 00 00\tcallq\t0x10 <target>\n 5: c3\tretq\n')
        result=tool.trace(code,[0],{16:'import'})
        self.assertEqual(result['reachable_pending_imports'][0]['instruction_path'],['0x0','0x10'])
        self.assertFalse(result['barriers'])
    def test_branches_cycles_and_unreachable_bytes(self):
        code={0:(b'xx','jne','0x8'),2:(b'xx','jmp','0x0'),8:(b'x','retq',''),
              9:(b'x','callq','0x10')}
        self.assertEqual(tool.trace(code,[0],{16:'unreachable'})['reachable_pending_imports'],[])
    def test_indirect_calls_are_barriers_but_continue(self):
        code={0:(b'xx','callq','*%rax'),2:(b'xx','jmpq','*%rbx')}
        result=tool.trace(code,[0],{})
        self.assertEqual(len(result['barriers']),2)
        self.assertEqual(result['barriers'][0]['instruction_path'],['0x0'])
        self.assertEqual(result['visited_instructions'],2)
    def test_relocated_rip_relative_dispatch_is_followed(self):
        code={0:(b'xx','jmpq','*0x100(%rip) # 0x108'),
              0x40:(b'x','retq','')}
        result=tool.trace(code,[0],{},indirect_targets={0x108:0x40})
        self.assertFalse(result['barriers'])
        self.assertEqual(result['visited_instructions'],2)
        self.assertEqual(result['resolved_indirect'],[dict(address='0x0',slot='0x108',target='0x40',relocation='R_X86_64_RELATIVE')])
    def test_reviewed_bounded_indirect_branch_enqueues_every_case(self):
        code={0:(b'xx','jmpq','*%rax'),0x10:(b'x','retq',''),0x20:(b'x','retq','')}
        result=tool.trace(code,[0],{},indirect_branches={0:[0x10,0x20]})
        self.assertFalse(result['barriers'])
        self.assertEqual(result['visited_instructions'],3)
        self.assertEqual(result['resolved_indirect'][0]['targets'],['0x10','0x20'])
    def test_relocated_plt_style_register_jump(self):
        code={0:(b'1234567','movq','0x100(%rip), %rax # 0x108'),
              7:(b'12','jmpq','*%rax'),0x20:(b'x','retq','')}
        jumps=tool.relocated_register_jumps(code,{0x108:0x20})
        self.assertEqual(jumps,{7:[0x20]})
        self.assertEqual(tool.trace(code,[7],{},indirect_branches=jumps)['barriers'],[])
    def test_missing_decode_and_budget(self):
        self.assertEqual(tool.trace({},[0],{})['barriers'][0]['reason'],'missing_instruction')
        with self.assertRaisesRegex(ValueError,'budget'):
            tool.trace({0:(b'x','nop','')},[0],{},limit=1)
    def test_segment_access_and_stop(self):
        result=tool.trace({0:(b'x','movq','%fs:0x0, %rax'),1:(b'x','ud2','')},[0],{})
        self.assertEqual(result['segment_accesses'],['0x0'])
        self.assertEqual(result['barriers'][0]['reason'],'ud2')
    def test_duplicate_instruction_rejected(self):
        with self.assertRaisesRegex(ValueError,'Duplicate'):
            tool.instructions('0: c3\tretq\n0: c3\tretq\n')


if __name__=='__main__':unittest.main()
