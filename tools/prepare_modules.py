"""Prepare unencrypted/uncompressed SELF modules and audit exact guest bindings.

Creates analysis artifacts only. Does not relocate, link, or execute guest code.
"""
import argparse
import collections
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import struct


def sibling(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name+'.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


elf_tool = sibling('inspect_imports')
span = elf_tool.span


def convert_self(data):
    """Copy data by SELF segment index, never by matching segment size.

    Only supports the unencrypted, uncompressed, non-sectioned layout observed
    locally. Every non-NULL program header file range must have verified backing.
    """
    if data[:4] == b'\x7fELF':
        return data, dict(format='ELF', copied_segments=[])
    if span(data, 0, 4) != bytes.fromhex('4f153d1d'):
        raise ValueError('Not an ELF or SELF')
    header = span(data, 0, 32)
    count = struct.unpack_from('<H', header, 24)[0]
    if not 0 < count <= 4096:
        raise ValueError('Unsupported SELF entry count')
    entries = list(struct.iter_unpack('<4Q', span(data, 32, count*32)))
    elf_start = 32 + count*32
    eh = span(data, elf_start, 64)
    if eh[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H', eh, 18)[0] != 62:
        raise ValueError('Unsupported embedded ELF')
    phoff = struct.unpack_from('<Q', eh, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', eh, 54)
    if phsize != 56 or phcount in (0, 65535) or struct.unpack_from('<H', eh, 60)[0]:
        raise ValueError('Unsupported ELF program/section layout')
    phbytes = span(data, elf_start + phoff, phcount*56)
    phs = list(struct.iter_unpack('<II6Q', phbytes))
    extent = max([64, phoff+len(phbytes)] + [p[2]+p[5] for p in phs if p[0] and p[5]])
    if extent > 256*1024*1024:
        raise ValueError('ELF exceeds conversion budget')
    output = bytearray(extent)
    written = []

    def write(offset, payload):
        span(output, offset, len(payload))
        end = offset + len(payload)
        for lo, hi in written:
            a, b = max(lo, offset), min(hi, end)
            if a < b and output[a:b] != payload[a-offset:b-offset]:
                raise ValueError('Conflicting overlapping ELF data')
        output[offset:end] = payload
        written.append((offset, end))

    write(0, eh)
    write(phoff, phbytes)
    copied, seen = [], set()
    for props, off, size, memory_size in entries:
        span(data, off, size)
        if not props & 0x800:
            continue  # Digest/extent metadata is not ELF segment payload.
        if props & 2:
            raise ValueError('Encrypted SELF segment is unsupported')
        if props & 8:
            raise ValueError('Compressed SELF segment is unsupported')
        index = (props >> 20) & 0xfff
        if index >= len(phs) or index in seen:
            raise ValueError('Invalid or duplicate SELF segment index')
        ph = phs[index]
        if size != ph[5] or memory_size != size:
            raise ValueError('SELF payload size does not match ELF file size')
        payload = span(data, off, size)
        write(ph[2], payload)
        copied.append(dict(index=index, size=size, sha256=hashlib.sha256(payload).hexdigest()))
        seen.add(index)
    # Observed container layout stores LIBVERSION as a verbatim tail after entries.
    tail_start = max(off+size for _, off, size, _ in entries)
    tail = data[tail_start:]
    versions = [p for p in phs if p[0] == 0x6fffff01 and p[5]]
    if versions:
        if len(versions) != 1 or versions[0][5] != len(tail):
            raise ValueError('Unsupported SELF version tail')
        write(versions[0][2], tail)
    for ph in phs:
        if not ph[0] or not ph[5]:
            continue
        cursor, end = ph[2], ph[2]+ph[5]
        for lo, hi in sorted(written):
            if lo <= cursor:
                cursor = max(cursor, hi)
        if cursor < end:
            raise ValueError('ELF program header has missing source bytes')
    return bytes(output), dict(format='SELF', copied_segments=copied)


def identity(symbol):
    if not symbol['library'] or not symbol['module']:
        return None
    return (symbol['nid'], symbol['library'], symbol['library_version'],
            symbol['module'], symbol['module_version'])


def bindings(modules):
    exports = collections.defaultdict(list)
    for module in modules:
        for symbol in module['inventory']['exports']:
            key = identity(symbol)
            if key:
                exports[key].append((module['name'], symbol))
    result = []
    for module in modules:
        for symbol in module['inventory']['imports']:
            providers = exports.get(identity(symbol), []) if identity(symbol) else []
            # Reject wrong type, absolute/section-special exports, and nonmapped targets.
            providers = [(name, export) for name, export in providers
                         if (symbol['kind'] == export['kind'] or 'notype' in (symbol['kind'], export['kind']))
                         and export['section'] < 0xff00]
            status = 'guest_candidate' if len(providers) == 1 else 'ambiguous' if providers else 'external_unresolved'
            result.append(dict(requester=module['name'], symbol_index=symbol['symbol_index'],
                               nid=symbol['nid'], library=symbol['library'], module=symbol['module'],
                               library_version=symbol['library_version'], module_version=symbol['module_version'],
                               kind=symbol['kind'], binding=symbol['binding'], status=status,
                               providers=[dict(file=name, symbol_index=e['symbol_index'], guest_va=hex(e['value'])) for name, e in providers]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, required=True)
    parser.add_argument('--eboot', type=Path, required=True)
    parser.add_argument('--elf-output', type=Path, required=True)
    parser.add_argument('--report-output', type=Path, required=True)
    parser.add_argument('--iss-source', type=Path)
    args = parser.parse_args()
    sources = [args.eboot] + sorted(args.game_root.rglob('*.prx'))
    args.elf_output.mkdir(parents=True, exist_ok=True)
    modules, names = [], set()
    # Parse every input before writing any generated modules.
    outputs = []
    for path in sources:
        name = path.stem + '.elf'
        if name.casefold() in names:
            raise ValueError('Duplicate output module name')
        names.add(name.casefold())
        source = path.read_bytes()
        elf, conversion = convert_self(source)
        inventory = elf_tool.inspect(elf)
        phoff = struct.unpack_from('<Q', elf, 32)[0]
        count = struct.unpack_from('<H', elf, 56)[0]
        phs = list(struct.iter_unpack('<II6Q', span(elf, phoff, count*56)))
        loads = [p for p in phs if p[0] == 1]
        for export in inventory['exports']:
            if export['section'] < 0xff00 and export['kind'] != 'tls':
                if not any(p[3] <= export['value'] and max(export['size'], 1) <= p[6] - (export['value']-p[3]) for p in loads):
                    raise ValueError(f'{name}: export outside PT_LOAD')
        modules.append(dict(name=name, source_name=path.name,
                            source_sha256=hashlib.sha256(source).hexdigest(),
                            conversion=conversion, inventory=inventory))
        outputs.append((name, elf))
    stale = [p.name for p in args.elf_output.glob('*.elf') if p.name.casefold() not in names]
    if stale:
        raise ValueError(f'Output directory contains unrelated ELF files: {stale}')
    rows = bindings(modules)
    if args.iss_source:
        iss = sibling('compare_iss_imports')
        known, entries = iss.scan(args.iss_source)
        lookup = {}
        for m in modules:
            for row in iss.compare(m['inventory']['imports'], known, entries):
                lookup[(m['name'], row['symbol_index'])] = row
        for row in rows:
            evidence = lookup[(row['requester'], row['symbol_index'])]
            row['function_name'] = evidence['name']
            row['iss_static_status'] = evidence['status']
            row['iss_evidence'] = evidence['evidence']
    for name, data in outputs:
        (args.elf_output/name).write_bytes(data)
    args.report_output.mkdir(parents=True, exist_ok=True)
    summary = {m['name']: dict(imports=len(m['inventory']['imports']), exports=len(m['inventory']['exports'])) for m in modules}
    report = dict(modules=modules, bindings=rows, summary=summary,
                  counts=dict(collections.Counter(r['status'] for r in rows)),
                  limitations=['Static candidates only: no relocations applied, no constructors or guest code executed.',
                               'Exact NID, library/module names and versions required; no version alias guessed.',
                               'Discovers supplied PRX files, not runtime load order or every possible SPRX.',
                               'TLS allocation, ABI and transitive host implementations still required.'])
    (args.report_output/'modules.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    with (args.report_output/'bindings.csv').open('w', newline='', encoding='utf-8-sig') as f:
        columns = ['requester','symbol_index','nid','library','module','status','function_name','iss_static_status','providers']
        writer = csv.DictWriter(f, fieldnames=columns, extrasaction='ignore')
        writer.writeheader()
        for row in rows:
            writer.writerow(dict(row, providers=json.dumps(row['providers'])))
    lines = ['# Módulos LBP3', '', 'Preparação e bindings estáticos; não são funções já ligadas em memória.', '',
             '| Arquivo | Imports | Exports | Candidatos guest | Externos sem binding guest |', '|---|---:|---:|---:|---:|']
    for name, values in summary.items():
        counts = collections.Counter(r['status'] for r in rows if r['requester'] == name)
        lines.append(f"| {name} | {values['imports']} | {values['exports']} | {counts['guest_candidate']} | {counts['external_unresolved']} |")
    external = [r for r in rows if r['status'] == 'external_unresolved']
    if args.iss_source:
        host_counts = collections.Counter(r['iss_static_status'] for r in external)
        lines += ['', f"Dos {len(external)} imports sem binding guest, {host_counts['candidate_found']} têm candidato estático no ISS, {host_counts['name_only']} apenas nome e {host_counts['not_found']} nenhuma correspondência."]
    lines += ['', '## Dependências externas por biblioteca', '', '| Biblioteca | Imports |', '|---|---:|']
    lines += [f'| {name or "(símbolo sem identidade de biblioteca)"} | {count} |' for name, count in collections.Counter(r['library'] for r in external).most_common()]
    lines += ['', '## Limites', '', 'Os candidatos exigem igualdade de NID, biblioteca, módulo e versões. Não foram aplicadas relocations ou chamadas entradas de módulos.',
              'Os estados ISS são somente evidências estáticas do relatório anterior e não comprovam implementação compatível.',
              'modules.json registra SHA-256 da origem, do ELF e dos segmentos copiados por índice SELF. Os originais são apenas lidos.',
              'A ordem de inicialização e o carregamento dinâmico ainda não foram determinados.']
    (args.report_output/'README.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    print(json.dumps(dict(modules=len(modules), counts=report['counts'], summary=summary), indent=2))


if __name__ == '__main__':
    main()
