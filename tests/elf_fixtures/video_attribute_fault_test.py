import subprocess
import sys
result=subprocess.run([sys.argv[1],'--invalid-output'],capture_output=True,timeout=10)
assert result.returncode & 0xffffffff == 0xe04c4205,(result.returncode,result.stderr)
assert b'sceVideoOutSetBufferAttribute output is inaccessible' in result.stderr,result.stderr
print('Invalid VideoOut attribute output terminates the diagnostic child explicitly.')
