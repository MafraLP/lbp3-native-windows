"""Verify the actual typed resolver call in a disposable child process."""
import subprocess
import sys

result = subprocess.run([sys.argv[1], '--stack-failure-fixture'],
                        capture_output=True, timeout=10)
assert result.returncode & 0xffffffff == 0xe04c4201, (result.returncode, result.stderr)
assert b'Guest stack canary failure (__stack_chk_fail)' in result.stderr, result.stderr
assert not result.stdout, result.stdout
print('Stack failure terminates child with expected diagnostic; no return or ELF unwinding.')
