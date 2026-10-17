import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('compare_iss', Path(__file__).resolve().parents[2] / 'tools/compare_iss_imports.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class Comparison(unittest.TestCase):
    def symbol(self, library='libTest'):
        return dict(symbol_index=1, nid='abc', library=library, module='test', kind='function')

    def test_name_alone_does_not_claim_implementation(self):
        row = tool.compare([self.symbol()], {'abc#libTest': 'test'}, [])[0]
        self.assertEqual(row['status'], 'name_only')
        self.assertFalse(row['evidence'])

    def test_exact_nid_is_library_scoped(self):
        entry = dict(method='runtime_exact_nid', key='abc#Other', target='target')
        row = tool.compare([self.symbol()], {}, [entry])[0]
        self.assertEqual(row['status'], 'not_found')

    def test_posix_alias(self):
        entry = dict(method='runtime_exact_nid', key='abc#libkernel', target='target')
        row = tool.compare([self.symbol('libScePosix')], {}, [entry])[0]
        self.assertEqual(row['status'], 'candidate_found')

    def test_comment_exclusion_preserves_lines(self):
        text = '/* fake\nexport */\nint real; // fake'
        clean = tool.strip_comments(text)
        self.assertNotIn('fake', clean)
        self.assertEqual(text.count('\n'), clean.count('\n'))

    def test_placeholder_and_real_body_differ(self):
        text = 'int offline(void) { return OFFLINE; }\nint work(void) { run(); return 0; }'
        self.assertEqual(tool.assess(text, 'offline'), 'constant_or_trivial_return_review_required')
        self.assertEqual(tool.assess(text, 'work'), 'body_present_semantics_unverified')


if __name__ == '__main__':
    unittest.main()
