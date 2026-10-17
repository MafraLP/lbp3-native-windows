import subprocess
import sys
import tempfile
from pathlib import Path

exe=sys.argv[1]
for mode,code,marker in [('trap',0xe04c4301,b'PROBE_UNIMPLEMENTED: fixture/fixture nid=SyntheticNID'),
                         ('data',0xe04c4303,b'PROBE_UNIMPLEMENTED_DATA: data fixture/fixture nid=SyntheticData'),
                         ('fault',0xe04c4302,b'PROBE_MACHINE_FAULT:')]:
    result=subprocess.run([exe,'--probe-fixture',mode],capture_output=True,timeout=10)
    assert result.returncode & 0xffffffff == code,(mode,result.returncode,result.stderr)
    assert marker in result.stderr,(mode,result.stderr)
with tempfile.TemporaryDirectory() as directory:
    # Missing images and arbitrary modules must fail before any init call.
    Path(directory,'unexpected.elf').write_bytes(b'not an ELF')
    for mode in ['--probe-pinned-libc','--probe-pinned-modules','--probe-pinned-constructors']:
        result=subprocess.run([exe,mode,directory],capture_output=True,timeout=10)
        assert result.returncode!=0 and b'PROBE_CALL' not in result.stdout,result
print('Probe traps, fault termination and rejection of unpinned input verified in children.')
