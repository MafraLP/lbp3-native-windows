import subprocess
import sys
result=subprocess.run([sys.argv[1],'--invalid-input'],capture_output=True,timeout=10)
assert result.returncode & 0xffffffff == 0xe04c4206,(result.returncode,result.stderr)
assert b'inaccessible kernel event accessor' in result.stderr,result.stderr
print('Invalid kernel event accessor stops the diagnostic child explicitly.')
