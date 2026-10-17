"""Enrich native pending relocation CSV with names from the static inventory."""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path


def enrich(rows, inventory):
    symbols, hints = {}, {}
    for module in inventory['modules']:
        for symbol in module['inventory']['imports'] + module['inventory']['exports']:
            symbols[module['name'], symbol['symbol_index']] = symbol
    for binding in inventory['bindings']:
        hints[binding['requester'], binding['symbol_index']] = binding
    result = []
    for row in rows:
        row = dict(row)
        row['symbol_index'] = int(row['symbol_index'])
        row['type'] = int(row['type'])
        key = row['requester'], row['symbol_index']
        symbol, hint = symbols.get(key, {}), hints.get(key, {})
        if symbol and symbol['raw_name'] != row['raw_name']:
            raise ValueError('Native symbol does not match inventory; regenerate the module inventory')
        row.update(library=symbol.get('library', ''), module=symbol.get('module', ''),
                   kind=symbol.get('kind', ''), function_name=hint.get('function_name', ''),
                   iss_static_status=hint.get('iss_static_status', 'not_checked'))
        result.append(row)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pending', type=Path, required=True)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text(encoding='utf-8'))
    with args.pending.open(encoding='utf-8-sig', newline='') as f:
        rows = enrich(list(csv.DictReader(f)), inventory)
    counts = dict(collections.Counter(row['reason'] for row in rows))
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output/'pending.csv').open('w', encoding='utf-8-sig', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]) if rows else ['requester'])
        writer.writeheader()
        writer.writerows(rows)
    report = dict(total=len(rows), reasons=counts, pending=rows,
                  source_csv_sha256=hashlib.sha256(args.pending.read_bytes()).hexdigest(),
                  module_inventory_sha256=hashlib.sha256(args.inventory.read_bytes()).hexdigest(),
                  limitations=['Counts are relocation records, not unique functions.',
                               'ISS status is static evidence only, not a bound host implementation.',
                               'ABI fixtures do not validate guest TLS, exceptions, varargs, aggregates or red-zone behavior.'])
    (args.output/'pending.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    lines = ['# Pendências nativas de ligação', '', f'{len(rows)} registros pendentes.', '',
             '| Motivo | Registros |', '|---|---:|']
    lines += [f'| {name} | {count} |' for name, count in sorted(counts.items())]
    unresolved = [r for r in rows if r['reason'] == 'unresolved_symbol']
    lines += ['', '## Símbolos não resolvidos por biblioteca', '', '| Biblioteca | Referências |', '|---|---:|']
    lines += [f'| {name or "(sem identidade)"} | {count} |' for name, count in collections.Counter(r['library'] for r in unresolved).most_common()]
    lines += ['', '## Limites e próximo trabalho', '',
              'Os símbolos não resolvidos incluem funções e dados; não são todos chamadas de função.',
              'Os serviços host habilitados são registrados separadamente no loader. As linhas deste relatório continuam pendentes. Nomes e candidatos ISS servem para análise, sem garantia de semântica ou ABI compatível.',
              'A ponte ABI e os blocos TLS foram testados com fixtures sintéticas; ainda faltam o acesso TLS pelo código original, exceções, varargs, agregados e validação de red zone.',
              'Próximo trabalho: iniciar serviços host com contratos explícitos e testes, mantendo símbolos desconhecidos pendentes.',
              f"Restam {counts.get('tls_relocation', 0)} relocations TLS e {counts.get('relative_nonzero_symbol', 0)} casos RELATIVE com símbolo não zero neste relatório."]
    (args.output/'README.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    print(json.dumps(dict(total=len(rows), reasons=counts)))


if __name__ == '__main__':
    main()
