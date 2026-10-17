"""Verify nonreturning diagnostic exports in disposable child processes."""
import subprocess
import sys

for mode,code,service,guest_code in [('debug',0xe04c4202,'sceKernelDebugRaiseException','a002000b'),
                                     ('release',0xe04c4203,'sceKernelDebugRaiseExceptionOnReleaseMode','a002000b'),
                                     ('exit',73,'_exit','00000049')]:
    result=subprocess.run([sys.argv[1],'--fatal-service-fixture',mode],capture_output=True,timeout=10)
    assert result.returncode & 0xffffffff == code,(mode,result.returncode,result.stderr)
    assert f'{service} code=0x{guest_code}'.encode() in result.stderr,(mode,result.stderr)
    assert not result.stdout,(mode,result.stdout)
print('Three fatal services terminate child processes with expected diagnostics and codes.')
