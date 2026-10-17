"""Offline Orbis ELF import inventory. No guest code is executed.

Format references (not copied implementation):
https://github.com/shadps4-emu/shadPS4/blob/main/src/core/loader/elf.h
https://github.com/shadps4-emu/shadPS4/blob/main/src/core/module.cpp
"""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
import struct


def span(data, offset, size):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError('Table range outside its containing data')
    return data[offset:offset + size]


def encoded_id(value):
    alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
    result = alphabet[value & 63]
    while value >= 64:
        value >>= 6
        result = alphabet[value & 63] + result
    return result


def inspect(data):
    header = span(data, 0, 64)
    if header[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H', header, 18)[0] != 62:
        raise ValueError('Expected little-endian ELF64 x86-64')
    phoff = struct.unpack_from('<Q', header, 32)[0]
    entsize, count = struct.unpack_from('<HH', header, 54)
    if entsize != 56 or count == 65535:
        raise ValueError('Unsupported program header layout')
    phs = list(struct.iter_unpack('<II6Q', span(data, phoff, count * 56)))

    def segment(kind):
        matches = [p for p in phs if p[0] == kind]
        if len(matches) != 1:
            raise ValueError(f'Expected one segment {kind:#x}')
        p = matches[0]
        return span(data, p[2], p[5])

    dynamic = segment(2)
    blob = segment(0x61000000)
    if len(dynamic) % 16:
        raise ValueError('Truncated dynamic entry')
    tags = collections.defaultdict(list)
    for tag, value in struct.iter_unpack('<qQ', dynamic):
        if tag == 0:
            break
        tags[tag].append(value)
    else:
        raise ValueError('Missing DT_NULL')

    def one(tag):
        values = tags[tag]
        if len(values) != 1:
            raise ValueError(f'Expected one dynamic tag {tag:#x}')
        return values[0]

    strings = span(blob, one(0x61000035), one(0x61000037))

    def string(offset):
        if offset >= len(strings):
            raise ValueError('String offset outside string table')
        end = strings.find(b'\0', offset)
        if end < 0:
            raise ValueError('Unterminated string')
        return strings[offset:end].decode('utf-8', errors='strict')

    def descriptors(tag):
        return [dict(name=string(v & 0xffffffff), id=v >> 48,
                     encoded_id=encoded_id(v >> 48), version=(v >> 32) & 0xffff,
                     raw_value=hex(v)) for v in tags[tag]]

    modules = descriptors(0x6100000f)
    libraries = descriptors(0x61000015)
    export_modules = descriptors(0x6100000d)
    export_libraries = descriptors(0x61000013)

    def identities(items):
        result = {}
        for item in items:
            key = item['encoded_id']
            if key in result and (result[key]['name'], result[key]['version']) != (item['name'], item['version']):
                raise ValueError('Conflicting module/library identity')
            result[key] = item
        return result

    module_ids = identities(modules + export_modules)
    library_ids = identities(libraries + export_libraries)
    if one(0x6100003b) != 24:
        raise ValueError('Unsupported symbol entry size')
    symbols_blob = span(blob, one(0x61000039), one(0x6100003f))
    if len(symbols_blob) % 24:
        raise ValueError('Truncated symbol table')
    symbols = list(struct.iter_unpack('<IBBHQQ', symbols_blob))
    imports = []
    exports = []
    by_index = {}
    for i, (name, info, other, section, value, size) in enumerate(symbols):
        if i == 0 or info >> 4 not in (1, 2):
            continue
        raw_name = string(name)
        parts = raw_name.split('#')
        nid, lib, mod = parts if len(parts) == 3 else (raw_name, '', '')
        if len(parts) == 3 and (lib not in library_ids or mod not in module_ids):
            raise ValueError('Symbol references undeclared library/module ID')
        row = dict(symbol_index=i, raw_name=raw_name, nid=nid,
                   library_id=lib, module_id=mod,
                   library=library_ids.get(lib, {}).get('name', ''),
                   module=module_ids.get(mod, {}).get('name', ''),
                   library_version=library_ids.get(lib, {}).get('version'),
                   module_version=module_ids.get(mod, {}).get('version'),
                   kind={0: 'notype', 1: 'object', 2: 'function', 6: 'tls'}.get(info & 15, str(info & 15)),
                   binding='weak' if info >> 4 == 2 else 'global',
                   relocation_count=0, function_name=None, value=value, size=size,
                   visibility=other & 3, section=section)
        if section == 0:
            imports.append(row)
            by_index[i] = row
        elif (other & 3) in (0, 3):
            exports.append(row)
    relocation_types = collections.Counter()
    for offset_tag, size_tag in [(0x61000029, 0x6100002d), (0x6100002f, 0x61000031)]:
        if not tags[offset_tag] and not tags[size_tag]:
            continue
        table = span(blob, one(offset_tag), one(size_tag))
        if len(table) % 24:
            raise ValueError('Truncated RELA table')
        for target, info, addend in struct.iter_unpack('<QQq', table):
            index, kind = info >> 32, info & 0xffffffff
            if index >= len(symbols):
                raise ValueError('Relocation references invalid symbol')
            relocation_types[kind] += 1
            if index in by_index:
                by_index[index]['relocation_count'] += 1
    return dict(sha256=hashlib.sha256(data).hexdigest(), modules=modules,
                libraries=libraries, needed_files=[string(v) for v in tags[1]],
                imports=imports, exports=exports, export_modules=export_modules,
                export_libraries=export_libraries, symbol_count=len(symbols),
                relocation_types=dict(sorted(relocation_types.items())),
                limitations=['Direct imports of this ELF only; excludes runtime-loaded and transitive dependencies.',
                             'NIDs retained verbatim; function names are not guessed.',
                             'Static inventory does not establish startup call order.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.elf.read_bytes())
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'imports.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    with (args.output / 'imports.csv').open('w', newline='', encoding='utf-8-sig') as f:
        columns = list(report['imports'][0]) if report['imports'] else ['symbol_index']
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        writer.writerows(report['imports'])
    counts = collections.Counter(row['library'] or '(unresolved library ID)' for row in report['imports'])
    lines = ['# Inventário de imports LBP3', '', f"ELF SHA-256: `{report['sha256']}`", '',
             f"{len(report['imports'])} símbolos importados; {len(report['modules'])} módulos; {len(report['libraries'])} bibliotecas.", '',
             '| Biblioteca | Símbolos importados |', '|---|---:|']
    lines += [f'| {name} | {count} |' for name, count in counts.most_common()]
    lines += ['', '## Módulos declarados', ''] + [f"- {m['name']} (ID {m['encoded_id']})" for m in report['modules']]
    lines += ['', '## Relocations por tipo', '', '```json', json.dumps(report['relocation_types'], indent=2), '```', '',
              '## Limites', '', 'Lista estática dos imports diretos deste ELF. Não inclui dependências transitivas nem bibliotecas carregadas durante a execução.',
              'As funções estão identificadas pelos NIDs originais em imports.csv. Os nomes legíveis ainda não foram resolvidos.',
              'A contagem não indica ordem de inicialização nem prioridade de implementação.', '',
              'Referências de formato: https://github.com/shadps4-emu/shadPS4/blob/main/src/core/loader/elf.h e https://github.com/shadps4-emu/shadPS4/blob/main/src/core/module.cpp']
    (args.output / 'README.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f"Saved {len(report['imports'])} imports, {len(report['modules'])} modules, {len(report['libraries'])} libraries to {args.output}")


if __name__ == '__main__':
    main()
