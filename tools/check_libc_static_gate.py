"""Fail visibly when the static requirements for controlled libc init are incomplete."""
import argparse
import hashlib
import json
from pathlib import Path


def check(audit, graph, elf_hash):
    if audit['sha256'] != elf_hash or graph['elf_sha256'] != elf_hash:
        raise ValueError('ELF/audit/graph hashes differ; regenerate the analysis')
    required={audit['lifecycle']['init']['guest_va']}
    required.update(slot['relative_guest_va'] for slot in audit['lifecycle']['preinit_array']['slots']
                    if slot['relative_guest_va'] is not None)
    required.update(audit['legacy_constructors']['call_order'])
    if not required.issubset(set(graph['roots'])):
        raise ValueError('Initialization graph is missing reviewed lifecycle roots')
    blockers=[]
    if graph['reachable_pending_imports']:
        blockers.append(f"{len(graph['reachable_pending_imports'])} potentially reachable imports remain unbound")
    if graph['barriers']:
        blockers.append(f"{len(graph['barriers'])} control-flow barriers remain unresolved")
    return blockers


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for option in ('elf','audit','graph','output','log'):
        parser.add_argument('--'+option,type=Path,required=True)
    args=parser.parse_args()
    audit=json.loads(args.audit.read_text(encoding='utf-8'))
    graph=json.loads(args.graph.read_text(encoding='utf-8'))
    elf_hash=hashlib.sha256(args.elf.read_bytes()).hexdigest()
    blockers=check(audit,graph,elf_hash)
    lines=['# Static libc initialization gate','',f'ELF SHA-256: `{elf_hash}`.','',
           'Result: **BLOCKED**.' if blockers else 'Result: static checks passed; runtime validation is still required.','']
    lines += ['- '+reason for reason in blockers]
    lines += ['','## Unbound imports in the reviewed graph','','| Import | NID | Call site |','|---|---|---|']
    for row in graph['reachable_pending_imports']:
        symbol=row['symbol'];path=row['instruction_path']
        lines.append(f"| {symbol['function_name'] or symbol['raw_name']} | {symbol['raw_name']} | {path[-2] if len(path)>1 else path[0]} |")
    lines += ['','The graph explores both conditional branches. These are potential paths, not observed calls.',
              'Indirect callbacks/jumps still prevent a complete proof. No real libc/game instructions were executed.',
              'Binding an export or modeling a signal mask does not validate signal delivery or exception unwinding.','',
              'A passed static gate alone must never enable execution automatically.']
    args.output.write_text('\n'.join(lines)+'\n',encoding='utf-8')
    if blockers:
        log_text = args.log.read_text(encoding='utf-8') if args.log.exists() else ''
        detail = ('\n## Static libc gate blocked\n\n'
                  'Command: `tools/check_libc_static_gate.py` with current ELF, startup.json and reachability.json.\n\n' +
                  '\n'.join('- '+reason for reason in blockers) + '\n\n' +
                  f'Details: `{args.output.as_posix()}`. Return code: 1. No ELF entry was called.\n' +
                  'Resume by resolving the listed contracts and indirect targets, then regenerate the graph and repeat this check.\n')
        if detail not in log_text:
            with args.log.open('a',encoding='utf-8') as log:
                log.write(detail)
    print('BLOCKED: '+'; '.join(blockers) if blockers else 'Static checks passed; no code executed.')
    return 1 if blockers else 0


if __name__=='__main__':
    raise SystemExit(main())
