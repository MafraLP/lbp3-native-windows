# Fundação ABI e classificação das pendências

Implementado `lbp3_host.exe --abi-self-test`, usando clang-cl x64 e funções
sintéticas `__attribute__((sysv_abi))`, com fronteiras `noinline` e chamadas por
ponteiros voláteis para impedir que o teste elimine as transições de ABI.

Verificações, em 1.000 repetições:

- Win64 chama uma função SysV com oito argumentos inteiros; esta chama Win64.
- Nove argumentos double cobrem registradores e pilha da ABI guest.
- Argumentos inteiros/double misturados cobrem distribuição diferente de argumentos.
- Uma função SysV chama Win64, que invoca callback SysV; resultado conferido.

Não é execução de instruções do LBP3 nem implementação de serviços do PS4.
As funções de teste são compiladas pelo próprio toolchain. Não foram verificados
TLS guest, exceções atravessando a fronteira, varargs, passagem de agregados,
todos os registradores preservados em código original ou proteção da red zone
contra o tratamento de exceções Windows. Não devem ser inferidos desses testes.

O loader agora exporta um CSV opcional dos registros pendentes:

```bat
out\build\win-clang-debug\lbp3_host.exe --link-set out\modules\lbp3 out\modules\pending.csv
```

Os 614 registros foram classificados pelo próprio caminho nativo de resolução:

| Motivo | Registros |
|---|---:|
| Símbolo não resolvido | 581 |
| Relocation TLS | 27 |
| RELATIVE com símbolo não zero | 6 |

Os registros continuam sem alterações. As 581 referências incluem funções e
dados; a contagem não representa funções únicas. O resumo com bibliotecas e
nomes conhecidos está em `analysis/imports/pending`, gerado por
`tools/summarize_pending.py`. Há hashes dos arquivos de entrada no JSON.

Validação: CTest inclui o teste ABI, as fixtures de aritmética, as fixtures de
linker e as anteriores de inspeção/mapeamento. As fixtures de linker também
conferem a contagem/motivo no CSV, além dos valores escritos na memória.

Próximo trabalho: implementar serviços host pequenos com contratos explícitos
e resolvers tipados, validar os bindings com fixtures sintéticas e só então
habilitar os NIDs correspondentes nos módulos. A base ISS continua como
referência; nenhum código do runtime ISS foi integrado nesta etapa.
