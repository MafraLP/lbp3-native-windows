"""Static evidence inventory, not a runtime compatibility test.

Reads a pinned ISS checkout and the JSON produced by inspect_imports.py.
Does not copy upstream implementations into the host or execute guest code.
"""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
import re


def strip_comments(text):
    # Preserve offsets and newlines so evidence line numbers remain reproducible.
    return re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: re.sub(r'[^\n]', ' ', m[0]), text, flags=re.S)


def function_body(text, name):
    match = re.search(r'\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', text)
    if not match:
        return ''
    start, depth = match.end(), 1
    for i in range(start, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i].strip()
    return ''


def assess(text, target):
    body = function_body(strip_comments(text), target)
    if not body:
        return 'body_not_located_review_required'
    if re.fullmatch(r'(?:\(void\)\s*\w+\s*;\s*)*return\s+[^;{}]+;\s*', body):
        return 'constant_or_trivial_return_review_required'
    if re.search(r'STUBBED|UNIMPLEMENTED|not implemented', body, re.I):
        return 'stub_marker_review_required'
    return 'body_present_semantics_unverified'


def scan(root):
    names_text = (root / 'port/src/import_names.inc').read_text(encoding='utf-8')
    names = dict(re.findall(r'\{\s*"([^"\n]+)"\s*,\s*"([^"\n]+)"\s*\}', strip_comments(names_text)))
    entries = []
    for path in sorted((root / 'port/src').glob('runtime*.c')):
        text = path.read_text(encoding='utf-8')
        clean = strip_comments(text)
        relative = path.relative_to(root).as_posix()
        for table in re.finditer(r'\bRuntimeExport\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\};', clean, re.S):
            for match in re.finditer(r'\{\s*"([^"\n]+)"\s*,\s*(\w+)\s*\}', table[1]):
                name, target = match.groups()
                line = clean[:table.start(1) + match.start()].count('\n') + 1
                entries.append(dict(method='runtime_name_table', key=name, target=target,
                                    path=relative, line=line, assessment=assess(text, target)))
        for match in re.finditer(r'if\s*\(\s*!strcmp\(name,\s*"([^"\n]+)"\)\)\s*return\s*\(uintptr_t\)\s*(&?\w+)\s*;', clean):
            key, target = match.groups()
            entries.append(dict(method='runtime_exact_nid', key=key, target=target,
                                path=relative, line=clean[:match.start()].count('\n')+1,
                                assessment='data_export_review_required' if target.startswith('&') else assess(text, target)))
    # Only libraries actually registered by tlggpu_init/register_kernel.
    gpu_files = ['gnmdriver/gnmdriver.cpp', 'videoout/video_out.cpp',
                 'kernel/equeue.cpp', 'avplayer/avplayer.cpp']
    for suffix in gpu_files:
        path = root / 'port/gpu/shadps4/core/libraries' / suffix
        text = path.read_text(encoding='utf-8')
        clean = strip_comments(text)
        for match in re.finditer(r'LIB_FUNCTION\(\s*"([^"\n]+)"\s*,\s*"([^"\n]+)"\s*,\s*\d+\s*,\s*"[^"\n]+"\s*,\s*(\w+)\s*\)', clean):
            nid, library, target = match.groups()
            entries.append(dict(method='gpu_nid_registration', key=nid+'#'+library, target=target,
                                path=path.relative_to(root).as_posix(),
                                line=clean[:match.start()].count('\n')+1, assessment=assess(text, target)))
            names.setdefault(nid+'#'+library, target)
    return names, entries


def compare(imports, names, entries):
    rows = []
    for symbol in imports:
        library = symbol['library']
        canonical = 'libkernel' if library == 'libScePosix' else library
        key = symbol['nid'] + '#' + canonical
        name = names.get(key) or names.get(symbol['nid']+'#'+library, '')
        matches = [dict(e) for e in entries if
                   (e['method'] == 'runtime_exact_nid' and e['key'] == key) or
                   (e['method'] == 'runtime_name_table' and name and e['key'] == name) or
                   (e['method'] == 'gpu_nid_registration' and e['key'] in (key, symbol['nid']+'#'+library))]
        status = 'candidate_found' if matches else 'name_only' if name else 'not_found'
        rows.append(dict(symbol_index=symbol['symbol_index'], nid=symbol['nid'],
                         library=library, module=symbol['module'], kind=symbol['kind'],
                         name=name, status=status, evidence=matches))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--imports', required=True, type=Path)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    inventory = json.loads(args.imports.read_text(encoding='utf-8'))
    names, entries = scan(args.source)
    rows = compare(inventory['imports'], names, entries)
    counts = collections.Counter(r['status'] for r in rows)
    manifest = {p.relative_to(args.source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in args.source.rglob('*') if p.is_file()}
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(upstream='https://github.com/HayatoG/iss_pc', commit=args.commit,
                  elf_sha256=inventory['sha256'], counts=dict(counts),
                  method='Static regex search of uncommented RuntimeExport tables, exact NID resolver branches, and four registered GPU/kernel library tables. No compile or runtime validation.',
                  limitations=['Candidate does not establish ABI, behavior, build reachability, resolver precedence, or LBP3 compatibility.',
                               'Trivial-return and stub labels are conservative heuristics requiring manual review.',
                               'Missing candidates may be served by guest PRX modules; they are not proven absent system-wide.',
                               'Export versions and dependency module closure have not been validated.'],
                  imports=rows, source_sha256=manifest)
    (args.output/'comparison.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    with (args.output/'comparison.csv').open('w', encoding='utf-8-sig', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=['symbol_index','nid','library','module','kind','name','status','evidence'])
        writer.writeheader()
        for row in rows:
            writer.writerow(dict(row, evidence='; '.join(f"{e['path']}:{e['line']} -> {e['target']} [{e['assessment']}]" for e in row['evidence'])))
    stats = collections.defaultdict(collections.Counter)
    for row in rows:
        stats[row['library']][row['status']] += 1
    lines = ['# Comparação LBP3 × ISS', '', f'Revisão ISS: `{args.commit}`.',
             f"ELF SHA-256: `{inventory['sha256']}`.", '',
             f"{len(rows)} imports: {counts['candidate_found']} com candidato no código, {counts['name_only']} somente com nome identificado e {counts['not_found']} sem correspondência.", '',
             '**São correspondências estáticas, não funções comprovadamente compatíveis.**', '',
             '| Biblioteca | Candidatos | Só nome | Não encontrado |', '|---|---:|---:|---:|']
    for library, values in sorted(stats.items()):
        lines.append(f"| {library} | {values['candidate_found']} | {values['name_only']} | {values['not_found']} |")
    lines += ['', '## Evidências e limites', '',
              'comparison.csv contém os nomes, alvos, arquivos e linhas. comparison.json inclui hashes dos fontes consultados.',
              'A busca considera tabelas RuntimeExport, retornos diretos do resolver e registros de GnmDriver, VideoOut, Equeue e AvPlayer.',
              'Comentários são excluídos da busca. A análise não executa o runtime, não verifica precedência de resolvers e não garante que cada candidato seja alcançável na build Windows.',
              'Retornos triviais e marcadores STUBBED são assinalados para revisão. Outros corpos também podem conter limitações.',
              'Ausências podem ser atendidas por módulos PRX do jogo; não significam que precisamos reimplementar tudo.',
              'O nome de uma função, isoladamente, não comprova que ela exista no runtime.', '',
              '## Revisão manual e integração', '',
              'Ver integration.md para licenças, build e sequência recomendada.']
    (args.output/'README.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    print(json.dumps(dict(total=len(rows), counts=dict(counts), names_identified=sum(bool(r['name']) for r in rows))))


if __name__ == '__main__':
    main()
