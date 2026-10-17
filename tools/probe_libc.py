"""Supervise the pinned libc-only diagnostic child and persist every result."""
import argparse
import datetime
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--exe',type=Path,required=True)
parser.add_argument('--modules',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--log',type=Path,required=True)
parser.add_argument('--timeout',type=int,default=20)
parser.add_argument('--dependencies',action='store_true',help='Also initialize/finalize the six pinned shared dependencies; never calls eboot entry.')
parser.add_argument('--constructors',action='store_true',help='Also call the audited void eboot DT_INIT/DT_FINI wrappers, after shared module initialization.')
parser.add_argument('--entry',action='store_true',help='Probe the audited executable entry in the isolated child.')
parser.add_argument('--gpu',action='store_true',help='Initialize the isolated ISS Vulkan backend before the audited entry; implies --entry.')
parser.add_argument('--app-root',type=Path)
args=parser.parse_args()
args.output.mkdir(parents=True,exist_ok=True)
mode='--probe-pinned-entry-gpu' if args.gpu else '--probe-pinned-entry' if args.entry else '--probe-pinned-constructors' if args.constructors else '--probe-pinned-modules' if args.dependencies else '--probe-pinned-libc'
command=[str(args.exe.resolve()),mode,str(args.modules.resolve())]
if args.entry or args.gpu:
    if not args.app_root:parser.error('--entry requires --app-root')
    save=args.output/'savedata';save.mkdir(exist_ok=True)
    command.extend([str(args.app_root.resolve()),str(save.resolve())])
try:
    result=subprocess.run(command,capture_output=True,timeout=args.timeout)
    out,err,code=result.stdout,result.stderr,result.returncode & 0xffffffff
except subprocess.TimeoutExpired as failure:
    out,err,code=failure.stdout or b'',failure.stderr or b'',0xffffffff
    err+=b'\nPROBE_TIMEOUT: child was killed by supervisor.\n'
args.output.joinpath('stdout.txt').write_bytes(out)
args.output.joinpath('stderr.txt').write_bytes(err)
lines=['','## Controlled libc diagnostic child '+datetime.datetime.now(datetime.timezone.utc).isoformat(),'',
       f'Command: `{subprocess.list2cmdline(command)}`',f'Result: `0x{code:08x}`. Timeout: {args.timeout}s.',
       f'Artifacts: `{args.output.as_posix()}/stdout.txt`, `stderr.txt`.','','```text',
       err.decode('utf-8',errors='replace')[-6000:],'```','']
with args.log.open('a',encoding='utf-8') as log:log.write('\n'.join(lines))
print(f'Pinned libc diagnostic child result: 0x{code:08x}')
print(err.decode('utf-8',errors='replace'))
print('\n'.join(out.decode('utf-8',errors='replace').splitlines()[-8:]))
raise SystemExit(0 if code==0 else 1)
