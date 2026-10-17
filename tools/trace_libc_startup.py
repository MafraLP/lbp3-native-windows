"""Conservative static instruction graph; never executes guest code."""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def instructions(text):
    result = {}
    for line in text.splitlines():
        match = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)\s*(\S+)(.*)$', line)
        if match:
            address = int(match[1], 16)
            raw = bytes.fromhex(match[2])
            if address in result:
                raise ValueError('Duplicate instruction address')
            result[address] = (raw, match[3], match[4].strip())
    return result


def relocated_register_jumps(code, relative_targets):
    """Resolve a single RIP-relative pointer load immediately feeding a JMP."""
    result = {}
    instruction_ends = {site + len(ins[0]):ins for site,ins in code.items()}
    for address, (_, mnemonic, operand) in code.items():
        if mnemonic not in ('jmp','jmpq') or not re.fullmatch(r'\*%[a-z0-9]+',operand):
            continue
        previous = instruction_ends.get(address)
        if not previous:
            continue
        _, prev_mnemonic, prev_operand = previous
        match = re.search(r'#\s*0x([0-9a-f]+)',prev_operand)
        if prev_mnemonic.startswith('mov') and match:
            slot = int(match[1],16)
            target = relative_targets.get(slot)
            if target is not None and prev_operand.split('#',1)[0].rstrip().endswith(operand[1:]):
                result[address] = [target]
    return result


def trace(code, roots, imports, limit=200000, nonreturning=(), indirect_targets=None,
          indirect_branches=None):
    queue = deque((root, None) for root in roots)
    indirect_targets = indirect_targets or {}
    indirect_branches = indirect_branches or {}
    parents, pending, barriers, tls, resolved_indirect = {}, {}, [], [], []
    while queue:
        address, parent = queue.popleft()
        if address in parents:
            continue
        if len(parents) >= limit:
            raise ValueError('Instruction graph budget exhausted')
        parents[address] = parent
        if address in imports:
            pending[address] = imports[address]
            continue
        instruction = code.get(address)
        if instruction is None:
            barriers.append(dict(address=hex(address), reason='missing_instruction'))
            continue
        raw, mnemonic, operand = instruction
        if '%fs:' in operand or '%gs:' in operand:
            tls.append(hex(address))
        next_address = address + len(raw)
        if mnemonic.startswith('ret'):
            continue
        if mnemonic in ('ud2', 'hlt', 'int3', '<unknown>'):
            barriers.append(dict(address=hex(address), reason=mnemonic))
            continue
        branch = mnemonic.startswith('j') or mnemonic.startswith('loop')
        call = mnemonic.startswith('call')
        if branch or call:
            target = re.match(r'^0x([0-9a-f]+)(?:\s|$)', operand)
            if target:
                destination=int(target[1], 16)
                queue.append((destination, address))
                if call and destination in nonreturning:
                    continue
            else:
                # RIP-relative absolute memory dispatch can be closed when its
                # pointer slot has a validated RELATIVE relocation in the ELF.
                slot = re.search(r'#\s*0x([0-9a-f]+)', operand)
                destination = indirect_targets.get(int(slot[1], 16)) if slot else None
                if destination is None:
                    destinations = indirect_branches.get(address)
                    if destinations:
                        for target_va in destinations:
                            queue.append((target_va, address))
                        resolved_indirect.append(dict(address=hex(address), targets=[hex(v) for v in destinations],
                                                      evidence='validated_static_dispatch'))
                    else:
                        barriers.append(dict(address=hex(address), reason='indirect_control_flow', instruction=mnemonic+' '+operand))
                else:
                    queue.append((destination, address))
                    resolved_indirect.append(dict(address=hex(address), slot=hex(int(slot[1], 16)),
                                                  target=hex(destination), relocation='R_X86_64_RELATIVE'))
                    if call and destination in nonreturning:
                        continue
            if mnemonic in ('jmp', 'jmpq'):
                continue
        queue.append((next_address, address))

    def path(address):
        result = []
        while address is not None:
            result.append(hex(address))
            address = parents[address]
        return list(reversed(result))
    for barrier in barriers:
        barrier['instruction_path'] = path(int(barrier['address'], 16))
    return dict(visited_instructions=len(parents), roots=[hex(root) for root in roots],
                root_paths={hex(root):path(root) for root in roots if root in parents},
                reachable_pending_imports=[dict(address=hex(address), symbol=value,
                                               instruction_path=path(address))
                                          for address, value in sorted(pending.items())],
                barriers=barriers, segment_accesses=tls,
                resolved_indirect=resolved_indirect)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('elf', 'disassembly', 'audit', 'output'):
        parser.add_argument('--'+option, type=Path, required=True)
    parser.add_argument('--additional-root',type=lambda value:int(value,0),action='append',default=[],
                        help='Callback VA manually established by disassembly review')
    parser.add_argument('--objdump',type=Path,help='Optional LLVM decoder for reachable instruction gaps')
    args = parser.parse_args()
    data = args.elf.read_bytes()
    audit = json.loads(args.audit.read_text(encoding='utf-8'))
    if hashlib.sha256(data).hexdigest() != audit['sha256']:
        raise ValueError('ELF does not match lifecycle audit')
    text = args.disassembly.read_text(encoding='utf-8-sig')
    code = instructions(text)
    offset = struct.unpack_from('<Q', data, 32)[0]
    count = struct.unpack_from('<H', data, 56)[0]
    phs = [struct.unpack_from('<II6Q',data,offset+i*56) for i in range(count)]
    loads = phs
    loads = [p for p in loads if p[0]==1 and p[1]&1]
    # Only RELATIVE relocations give an image-independent target without
    # consulting mutable guest state or guessing a symbol binding.
    proc = next(p for p in phs if p[0] == 0x61000000)
    dynamic = next(p for p in phs if p[0] == 2)
    tags = {}
    for tag, value in struct.iter_unpack('<QQ', data[dynamic[2]:dynamic[2]+dynamic[5]]):
        if tag == 0:
            break
        tags.setdefault(tag, []).append(value)
    relative_targets = {}
    for offset_tag, size_tag in ((0x61000029,0x6100002d),(0x6100002f,0x61000031)):
        if not tags.get(offset_tag) or not tags.get(size_tag):
            continue
        table = data[proc[2]+tags[offset_tag][0]:proc[2]+tags[offset_tag][0]+tags[size_tag][0]]
        if len(table) != tags[size_tag][0] or len(table) % 24:
            raise ValueError('Malformed relocation table while resolving indirect targets')
        for slot, info, addend in struct.iter_unpack('<QQq', table):
            if info & 0xffffffff == 8 and info >> 32 == 0:
                relative_targets[slot] = addend
    # Resolve tiny PLT-style stubs where one validated RIP-relative load is
    # immediately followed by a jump through the loaded register.
    relocated_jumps = relocated_register_jumps(code,relative_targets)
    # Reviewed switch tables: each guard bounds the index before a signed
    # 32-bit relative table lookup and indirect jump. All targets are checked.
    def va_to_file(va, size):
        for segment in phs:
            if segment[0] == 1 and va >= segment[3] and va-segment[3] <= segment[5]-size:
                return segment[2] + va-segment[3]
        raise ValueError(f'VA outside file-backed PT_LOAD: {va:#x}')
    switch_targets = {}
    if audit['sha256'] == '9811633cd5d49ae461ec0550468db5ef037d576abe1d732348844b930f397566':
        switch_specs = [
            # dispatch, table, entries, (guard, guard operand), lea, load, add
            (0x19008,0x1982c,33,(0x18ff1,'$0x20, %eax'),0x18ffa,0x19001,0x19005),
            (0x19597,0x198b0,10,(0x19580,'$0x9, %eax'),0x19589,0x19590,0x19594),
            (0x561fa,0x5644c,8,(0x561e3,'$0x7, %eax'),0x561ec,0x561f3,0x561f7),
            (0x4e755,0x4ef18,8,(0x4e736,'$0x7, %rax'),0x4e740,0x4e747,0x4e74b),
        ]
        for dispatch_va,table_va,count_entries,guard,lea_va,load_va,add_va in switch_specs:
            dispatch, guard_ins, lea_ins, load_ins, add_ins = (code.get(dispatch_va),code.get(guard[0]),
                code.get(lea_va),code.get(load_va),code.get(add_va))
            if (not all((dispatch,guard_ins,lea_ins,load_ins,add_ins)) or
                guard_ins[1] not in ('cmpl','cmpq') or guard_ins[2].split('#',1)[0].rstrip()!=guard[1] or
                lea_ins[1]!='leaq' or f'0x{table_va-(lea_va+len(lea_ins[0])):x}(%rip)' not in lea_ins[2] or
                load_ins[1]!='movslq' or load_ins[2]!='(%rcx,%rax,4), %rax' or
                add_ins[1]!='addq' or add_ins[2]!='%rcx, %rax' or
                dispatch[1] not in ('jmp','jmpq') or dispatch[2]!='*%rax'):
                raise ValueError(f'Reviewed libc switch dispatch no longer matches ELF at {dispatch_va:#x}')
            table_off=va_to_file(table_va,count_entries*4)
            table=data[table_off:table_off+count_entries*4]
            targets=[]
            for (displacement,) in struct.iter_unpack('<i',table):
                target_va=table_va+displacement
                if not any(p[0]==1 and p[1]&1 and p[3]<=target_va<p[3]+p[5] for p in phs):
                    raise ValueError(f'Jump-table target outside executable PT_LOAD: {target_va:#x}')
                if target_va not in code:
                    raise ValueError(f'Jump-table target lacks validated instruction: {target_va:#x}')
                targets.append(target_va)
            switch_targets[dispatch_va]=sorted(set(targets))
    def validate(decoded):
        for address, (raw, _, _) in decoded.items():
            if not any(address>=p[3] and address-p[3]+len(raw)<=p[5] and
                       data[p[2]+address-p[3]:p[2]+address-p[3]+len(raw)]==raw for p in loads):
                raise ValueError(f'Disassembly bytes differ from executable ELF at {address:#x}')
    validate(code)
    roots = [int(audit['lifecycle']['init']['guest_va'],16)]
    roots += [int(slot['relative_guest_va'],16) for slot in audit['lifecycle']['preinit_array']['slots']
              if slot['relative_guest_va'] is not None]
    roots += [int(value,16) for value in audit['legacy_constructors']['call_order']]
    roots += args.additional_root
    pending = {r['symbol_index']:r for r in audit['pending_relocations']}
    imports = {int(t['guest_va'],16):pending[t['symbol_index']] for t in audit['plt_thunks']
               if t['symbol_index'] in pending}
    # All imported thunks are graph boundaries, including already bound ones.
    boundaries = {int(t['guest_va'],16):t for t in audit['plt_thunks']}
    # Our stack-failure binding terminates the process; _exit is noreturn too.
    nonreturning={address for address,t in boundaries.items()
                  if t['symbol_name'].split('#')[0] in ('Ou3iL1abvng','6Z83sYWFlA8','OMDRKKAZ8I4','zE-wXIZjLoM')}
    nonreturning_wrappers=[]
    # 0x28890 is a 16-byte fatal-error shim: it sets the error code and calls
    # the already-reviewed nonreturning import at 0x100. The next bytes start
    # a distinct function; treating the shim as returning walks a literal pool.
    if audit['sha256']=='9811633cd5d49ae461ec0550468db5ef037d576abe1d732348844b930f397566':
        wrapper=[code.get(a) for a in (0x28890,0x28891,0x28894,0x28899,0x2889b)]
        if (all(wrapper) and wrapper[0][1:]==( 'pushq','%rbp') and
            wrapper[1][1:]==('movq','%rsp, %rbp') and
            wrapper[2][1]=='movl' and wrapper[2][2].split('#',1)[0].rstrip()=='$0xa002000b, %edi' and
            wrapper[3][1:]==('xorl','%esi, %esi') and
            wrapper[4][1:]==('callq','0x100 <PT_LOAD#0+0x100>') and
            0x100 in nonreturning):
            nonreturning.add(0x28890)
            nonreturning_wrappers.append(dict(entry='0x28890',call='0x2889b',target='0x100',evidence='verified_fatal_import'))
    decoded_gaps=[]
    for attempt in range(17):
        result=trace(code, roots, boundaries,nonreturning=nonreturning,
                     indirect_targets=relative_targets,
                     indirect_branches={**relocated_jumps,**switch_targets})
        gaps=[int(row['address'],16) for row in result['barriers'] if row['reason']=='missing_instruction']
        if not args.objdump or not gaps:
            break
        if attempt==16:
            raise ValueError('Reachable decode-gap budget exhausted')
        for address in gaps:
            load=next((p for p in loads if p[3]<=address<p[3]+p[5]),None)
            if load is None:
                raise ValueError(f'Reachable instruction outside executable ELF: {address:#x}')
            stop=min(address+4096,load[3]+load[5])
            region=data[load[2]+address-load[3]:load[2]+stop-load[3]]
            # llvm-objdump linearly decodes sectionless program segments from
            # their beginning, so it misses valid overlapping function entry
            # points. Build a temporary minimal PT_LOAD rooted at this exact
            # validated gap and bias the decoded addresses back to guest VAs.
            header=bytearray(64);header[:7]=b'\x7fELF\x02\x01\x01'
            struct.pack_into('<HHIQQQIHHHHHH',header,16,2,62,1,0,64,0,0,64,56,1,0,0,0)
            program=struct.pack('<II6Q',1,5,120,0,0,len(region),len(region),0x1000)
            with tempfile.TemporaryDirectory(prefix='lbp3-decode-') as temp_dir:
                temp_elf=Path(temp_dir)/'gap.elf'
                temp_elf.write_bytes(header+program+region)
                decoded=subprocess.run([str(args.objdump),'-d',f'--adjust-vma={address}',str(temp_elf)],
                                       capture_output=True,text=True,check=True,timeout=30)
            extra=instructions(decoded.stdout)
            if address not in extra:
                raise ValueError(f'Could not decode reachable instruction {address:#x}')
            validate(extra);code.update(extra);decoded_gaps.append(hex(address))
    result['decoded_gaps']=decoded_gaps
    result['nonreturning_imports']=[hex(address) for address in sorted(nonreturning)]
    result['nonreturning_wrappers']=nonreturning_wrappers
    result['reachable_pending_imports'] = [row for row in result['reachable_pending_imports']
                                           if int(row['address'],16) in imports]
    for row in result['reachable_pending_imports']:
        row['symbol'] = imports[int(row['address'],16)]
    result.update(elf_sha256=audit['sha256'],disassembly_sha256=hashlib.sha256(args.disassembly.read_bytes()).hexdigest(),
                  limitations=['Both sides of conditional branches are explored without dataflow or execution.',
                               'Only RIP-relative indirect dispatches with RELATIVE relocations are resolved; register/table callbacks, exception edges and runtime-generated code are not.',
                               'This is potential reachability, not proof that an import executes or that initialization is safe.'])
    args.output.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(dict(visited=result['visited_instructions'],pending=len(result['reachable_pending_imports']),
                         barriers=len(result['barriers']),resolved_indirect=len(result['resolved_indirect']),
                         segment_accesses=len(result['segment_accesses']))))


if __name__=='__main__':
    main()
