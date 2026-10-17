# Execução controlada de ELF sintético: concluída

O novo teste gera arquivos ELF64 Orbis x86-64 com instruções explicitamente
construídas em `src/loader/synthetic_elf.cpp`. Não usa o compilador para gerar
uma função host que apenas simule a entrada: as instruções são lidas do arquivo,
mapeadas pelo loader e chamadas no endereço ELF relocável da imagem carregada.

## Caminho testado

1. Gerar ELF, com segmentos RX/RW, PT_TLS, tabelas dinâmicas e imports reais.
2. Abrir e mapear pelos mesmos caminhos usados nas inspeções LBP3.
3. Resolver DTPMOD64, RELATIVE, __tls_get_addr e sched_yield. Exigir zero
   registros pendentes antes da primeira escrita e antes de executar.
4. Copiar o template TLS após relocations e criar uma thread pelo caminho
   scePthreadCreate/Join, com TLS e runtime próprios.
5. Um wrapper SysV controlado chama a entrada do ELF sob um guard SEH estreito.
6. O código carregado consulta TLS por import, confere inicialização/zeros,
   altera seu byte e chama sched_yield. Retorna 73 (byte 0x42 + 7).
7. O wrapper confere TLS final 0x55, o join recolhe o retorno e os recursos são liberados.

## Falha deliberada

Um segundo ELF contém uma função folha `xor eax,eax; mov rax,[rax]; ret`.
A leitura nula ocorre em ELF VA 0x1002. O guard só captura ACCESS_VIOLATION
com RIP dentro do intervalo da fixture; falhas fora dele continuam a busca
normal de handlers, sem serem disfarçadas como sucesso.

O teste exige código 0xc0000005, RIP correspondente a 0x1002, leitura no
endereço zero e TLS ainda válido na thread depois da captura. O arquivo
`synthetic-fault.json` registra código, RIP host, VA guest, endereço acessado,
operação e ID de thread. Não é um minidump ou coletor completo de crashes.

Uma terceira fixture troca um NID por um desconhecido. O linker exige resolução
completa e recusa essa imagem antes das escritas de relocation/execução.

## Resultado

Build clang-cl x64 e nove conjuntos CTest passaram. A saída registra:

```text
Synthetic ELF executed: result=73, TLS=0x55, imported host calls verified.
Synthetic fault captured: code=0xc0000005 guest_va=0x1002 access=read address=0.
Unresolved synthetic ELF rejected before execution and relocation writes.
ELF execution self-test passed.
```

O JSON produzido também foi lido e validado como JSON com a VA esperada.
Artefatos CTest: `out/build/win-clang-debug/synthetic-execution/`.

## Limites

O comando executa apenas fixtures fixas geradas por ele, sem rota para rodar
um ELF arbitrário. O wrapper e os serviços host continuam compilados pelo nosso
toolchain. A fixture de falha é folha, sem alterações de pilha; esse teste não
comprova unwinding de funções guest gerais, exceções C++, red zone ou tratamento
de falhas durante callbacks host. Não há watchdog fora do timeout CTest.

Ainda não foram executados construtores, entrada ou funções do LBP3. As 573
pendências do jogo não mudaram. O resultado valida o caminho básico de execução
CPU/memória/TLS/imports e um diagnóstico controlado, não a compatibilidade do jogo.
