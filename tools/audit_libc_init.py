"""Static lifecycle audit for a sectionless Orbis ELF; never executes it."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location('inspect_imports', Path(__file__).with_name('inspect_imports.py'))
elf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(elf)


def legacy_constructors(last_slot, pointer_at, executable, max_slots=256):
    """Review a disassembly-confirmed reverse list; no guessed start or execution."""
    if last_slot < 0 or last_slot % 8:
        raise ValueError('Unaligned legacy constructor slot')
    slots, calls = [], []
    for i in range(max_slots):
        slot = last_slot - i*8
        if slot < 0:
            break
        pointer = pointer_at(slot)
        slots.append(pointer)
        relocation = pointer['relocation']
        raw = int(pointer['raw'], 16)
        if relocation is None and raw == 0xffffffffffffffff:
            return dict(last_slot=hex(last_slot), sentinel_slot=hex(slot),
                        slots=slots, call_order=calls)
        if relocation is None and raw == 0:
            continue
        # Absolute raw values and unresolved symbols cannot establish a safe call.
        if relocation is None or relocation['type'] != 8:
            raise ValueError('Legacy constructor pointer is not a relative relocation')
        target = relocation['addend']
        if not executable(target):
            raise ValueError('Legacy constructor outside executable data')
        calls.append(hex(target))
    raise ValueError('Missing bounded legacy constructor sentinel')


def audit(data, legacy_last_slot=None, heap_table_va=None):
    inventory = elf.inspect(data)
    phoff = struct.unpack_from('<Q', data, 32)[0]
    count = struct.unpack_from('<H', data, 56)[0]
    phs = list(struct.iter_unpack('<II6Q', elf.span(data, phoff, count*56)))
    dp = next(p for p in phs if p[0] == 2)
    bp = next(p for p in phs if p[0] == 0x61000000)
    tags = {}
    for tag, value in struct.iter_unpack('<QQ', elf.span(data,dp[2],dp[5])):
        if not tag:
            break
        tags.setdefault(tag, []).append(value)

    def one(tag):
        values = tags.get(tag, [])
        if len(values) > 1:
            raise ValueError('Duplicate lifecycle tag')
        return values[0] if values else None

    def va_bytes(va, size):
        for p in phs:
            if p[0] == 1 and va >= p[3] and va-p[3] <= p[6] and size <= p[6]-(va-p[3]):
                delta = va-p[3]
                count = max(0,min(size,p[5]-delta))
                return elf.span(data,p[2]+delta,count)+bytes(size-count)
        raise ValueError('Lifecycle address outside PT_LOAD')

    blob = elf.span(data,bp[2],bp[5])
    symbols = list(struct.iter_unpack('<IBBHQQ',elf.span(blob,one(0x61000039),one(0x6100003f))))
    strings = elf.span(blob,one(0x61000035),one(0x61000037))
    relocations = []
    for ot, st in [(0x6100002f,0x61000031),(0x61000029,0x6100002d)]:
        if one(ot) is None:
            continue
        for target, info, addend in struct.iter_unpack('<QQq',elf.span(blob,one(ot),one(st))):
            index = info >> 32
            symbol = symbols[index]
            name = strings[symbol[0]:].split(b'\0',1)[0].decode('utf-8')
            relocations.append(dict(target=target, type=info & 0xffffffff, symbol_index=index,
                                    symbol_name=name, symbol_value=symbol[4], addend=addend))
    by_target = {r['target']:r for r in relocations}

    def pointer_at(va):
        raw = struct.unpack('<Q',va_bytes(va,8))[0]
        relocation = by_target.get(va)
        return dict(slot=hex(va),raw=hex(raw),relocation=relocation,
                    relative_guest_va=hex(relocation['addend']) if relocation and relocation['type']==8 else None)

    lifecycle = {}
    for name, tag in [('init',12),('fini',13)]:
        value = one(tag)
        if value is not None:
            if not any(p[0]==1 and p[1]&1 and p[3]<=value<p[3]+p[5] for p in phs):
                raise ValueError('Lifecycle entry is outside file-backed executable data')
        lifecycle[name] = dict(present=value is not None, guest_va=hex(value) if value is not None else None)
    for name, pointer_tag, size_tag in [('preinit_array',32,33),('init_array',25,27),('fini_array',26,28)]:
        va, size = one(pointer_tag), one(size_tag)
        if (va is None) != (size is None):
            raise ValueError('Incomplete lifecycle array metadata')
        if size is not None and (size%8 or size>1024*1024):
            raise ValueError('Invalid lifecycle array size')
        lifecycle[name] = dict(present=va is not None, guest_va=hex(va) if va is not None else None,
                               size=size, slots=[pointer_at(va+i) for i in range(0,size or 0,8)])
    thunks=[]
    for load in phs:
        if load[0]!=1 or not load[1]&1:
            continue
        for delta in range(0,min(load[5],4096)-5,16):
            va=load[3]+delta
            instruction=va_bytes(va,6)
            if instruction[:2]!=b'\xff\x25':
                continue
            slot=va+6+struct.unpack_from('<i',instruction,2)[0]
            relocation=by_target.get(slot)
            if relocation:
                thunks.append(dict(guest_va=hex(va),slot=hex(slot),symbol_index=relocation['symbol_index'],
                                   symbol_name=relocation['symbol_name']))
    result = dict(sha256=hashlib.sha256(data).hexdigest(), lifecycle=lifecycle, plt_thunks=thunks,
                relative_with_nonzero_symbol=[r for r in relocations if r['type']==8 and r['symbol_index']],
                imports=inventory['imports'], modules=inventory['modules'])
    if legacy_last_slot is not None:
        result['legacy_constructors'] = legacy_constructors(legacy_last_slot,pointer_at,
            lambda va: any(p[0]==1 and p[1]&1 and p[3]<=va<p[3]+p[5] for p in phs))
    if heap_table_va is not None:
        result['heap_api_table'] = [pointer_at(heap_table_va+i*8) for i in range(10)]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf',type=Path,required=True)
    parser.add_argument('--pending',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--legacy-last-slot',type=lambda value:int(value,0),
                        help='Reverse constructor list last slot confirmed by disassembly')
    parser.add_argument('--heap-table-va',type=lambda value:int(value,0),
                        help='Ten-pointer heap table address confirmed by disassembly')
    args = parser.parse_args()
    report = audit(args.elf.read_bytes(), args.legacy_last_slot, args.heap_table_va)
    pending = json.loads(args.pending.read_text(encoding='utf-8'))
    rows = [r for r in pending['pending'] if r['requester']==args.elf.name]
    report['pending_relocations'] = rows
    report['unresolved_symbols'] = sorted({r['function_name'] or r['raw_name'] for r in rows})
    names={r['symbol_index']:r['function_name'] for r in rows}
    for thunk in report['plt_thunks']:
        thunk['function_name']=names.get(thunk['symbol_index'],'')
    report['limitations'] = ['Static metadata only, not initialization call order or reachability.',
                             'DT_INIT present with VA zero is a valid entry; absence is different.',
                             'Caller parameters, indirect callbacks and legacy constructor lists require disassembly review.']
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(dict(lifecycle=report['lifecycle'],nonzero_relative=len(report['relative_with_nonzero_symbol']),
                          pending_records=len(rows),unresolved_symbols=len(report['unresolved_symbols'])),indent=2))


if __name__=='__main__':
    main()
