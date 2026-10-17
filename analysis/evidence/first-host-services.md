# Primeiros serviços host conectados

Implementados sem copiar fontes do runtime ISS. Os nomes/NIDs e assinaturas
foram confrontados com os registros e contratos consultados na auditoria fixa
`a065f561aef8544df2d0cf711dea93881afd04c3` do ISS, principalmente
`port/src/import_names.inc` e `port/src/runtime_kernel.c`.

| Serviço | NID | Implementação Windows |
|---|---|---|
| sceKernelUsleep(uint32_t microseconds) | 1jfXLRVzisc | Timer relativo one-shot, espera não alertável; zero cede via Sleep(0) |
| sceKernelGetProcessTime() -> uint64_t | 4J2sUJmuHZQ | QueryPerformanceCounter, microssegundos desde initialize() |
| sched_yield() -> int32_t | 6XG4B33N09g | SwitchToThread; retorna zero mesmo sem outra thread pronta |

Todos os pontos de entrada usam `sysv_abi` e não lançam exceções C++.
O resolver só aceita funções STT_FUNC de `libkernel`, módulo `libkernel`,
versão de biblioteca 1 e versão de módulo 0x0101. O aplicativo inicializa o
relógio uma vez na thread de controle antes de fornecer o resolver ao loader.

O resolver é injetado por callback, preservando a separação loader/HLE.
Exports guest têm prioridade sobre o fallback host. Não há resolução por
nome aproximado, stubs de sucesso para imports desconhecidos ou troca global
de convenção de chamada.

Resultado com os oito módulos locais:

- 30.879 RELATIVE e 1.313 referências locais aplicadas.
- 683 conexões guest preservadas.
- 4 referências host conectadas: usleep no eboot e Fios2, relógio e yield na libc.
- 610 pendentes: 577 símbolos, 27 TLS e 6 RELATIVE especiais.

Limites explícitos: a precisão do sleep depende do agendador Windows e pode
exceder o intervalo solicitado; interrupções por sinais guest e cancelamento
de threads ainda não existem. Falhas de timer/espera retornam SCE EIO
(0x80020005); não há tradução exaustiva de erros Windows. O relógio mede tempo
monotônico desde a inicialização do runtime, não consumo de CPU. Estes serviços
não resolvem a compatibilidade das threads, TLS, exceções ou red zone do jogo.
O código original do LBP3 continua sem execução.

Validação:

- `--hle-self-test` chama os endereços retornados pelo resolver usando assinaturas
  SysV reais, confere sleep zero, yield, espera de 5ms, unidades do relógio com
  leituras QPC que delimitam cada chamada e monotonicidade em 1.000 leituras.
- Testa recusa de NID, biblioteca, módulo, versões e tipo incorretos.
- Fixtures de linker conectam um NID host em três destinos sintéticos e deixam
  a versão incompatível pendente. Não executam instruções dos módulos.
- Todos os cinco conjuntos CTest passaram.

Comandos:

```bat
out\build\win-clang-debug\lbp3_host.exe --hle-self-test
out\build\win-clang-debug\lbp3_host.exe --link-set out\modules\lbp3 out\modules\pending.csv
```
