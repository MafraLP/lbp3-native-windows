# Dependências potenciais dos construtores da libc

Resultado desta etapa: ferramenta de rastreamento estático com testes;
implementados `__error` e `getpagesize`; **16/16 conjuntos CTest passaram**.
Nenhum código da libc original ou do jogo foi executado.

## Rastreamento reproduzível

`tools/trace_libc_startup.py` usa disassembly LLVM e as raízes já auditadas:
DT_INIT (VA zero), o alvo de PREINIT_ARRAY e os 21 construtores legados.
Antes da análise confere o SHA-256 do ELF contra `startup.json` e cada
sequência de bytes decodificada contra um PT_LOAD executável do ELF.
Segue chamadas diretas, fallthrough e ambos os lados de branches condicionais.
Retornos terminam o percurso; imports são fronteiras e ciclos são limitados
por visita única. Um orçamento impede crescimento ilimitado do grafo.

O resultado `reachability.json` contém um caminho de instruções para cada
import pendente encontrado e os pontos onde a análise para. Não resolve
fluxo de dados, tabelas de saltos, callbacks indiretos, unwinding/exceções
ou código criado em runtime. Portanto, os caminhos são **possíveis**, não
prova de execução ou de necessidade incondicional de cada serviço.
Chamadas indiretas são registradas como barreira, mas o retorno possível
ao chamador também é explorado. Saltos indiretos terminam aquele percurso.

Nesta imagem: 17.658 endereços visitados, **18 imports pendentes potencialmente
alcançáveis** e **94 barreiras**. Não foram encontrados acessos FS/GS no
subgrafo analisado; isso não comprova ausência fora dele e não valida TLS
do bootstrap completo. A closure continua incompleta.

Pendências do subgrafo:

- sceKernelDebugRaiseException, sceKernelDebugRaiseExceptionOnReleaseMode;
- sceKernelWrite;
- scePthreadCondWait, scePthreadCondSignal, scePthreadOnce,
  scePthreadKeyCreate, scePthreadSetspecific;
- signal, sigfillset, sigprocmask, sigreturn-like `_is_signal_return`;
- sceKernelGetModuleInfoFromAddr, __elf_phdr_match_addr, __pthread_cxa_finalize;
- _exit, mmap e munmap (exports POSIX distintos dos serviços SCE já implementados).

Essa lista não substitui as 45 referências pendentes totais da libc: apenas
representa o subconjunto encontrado por esse percurso estático. Outros
imports e alvos indiretos ainda podem ser necessários. Os 94 pontos não
são 94 APIs: incluem saltos/chamadas indiretos e outras interrupções do
percurso, detalhadas individualmente no JSON.

## Serviços implementados

| Serviço | NID libkernel | Contrato |
|---|---|---|
| __error | 9BcDykPmo1I | Retorna endereço estável de um int32 errno por thread host |
| getpagesize | k+AXqu2-eBc | Retorna int32 0x4000 (16 KiB guest) |

NIDs, assinatura e modelo foram comparados com referência primária
[shadPS4 kernel.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/kernel.cpp),
consultada em 2026-10-16; implementação própria, sem copiar código externo.
O resolver exige biblioteca/módulo libkernel, versão de biblioteca 1,
versão de módulo 0x0101 e tipo função.

O errno usa armazenamento thread_local do host, separado tanto do errno
do CRT Windows quanto dos templates PT_TLS do jogo. Não há modificação
manual de FS/GS/TEB. O ponteiro só pode ser usado durante a vida da thread
que o recebeu. Threads novas começam com zero; a thread host atual mantém
o valor até ele ser escrito novamente ou a thread terminar. Reutilizar uma
thread host para outra identidade guest exigirá política adicional de reset.
As APIs SCE atuais continuam retornando seus códigos diretamente; elas não
passam a modificar errno implicitamente. Futuros wrappers POSIX devem
traduzir erro e atualizar esse slot explicitamente.

Testes via endereços do resolver: três threads simultâneas com endereços
distintos, valores independentes e ponteiros estáveis; nova thread começa
com zero; getter não altera errno; escrita guest não altera errno nativo;
getpagesize consistente e rejeição de identidades/tipos/versões incorretos.
Fixtures de linker conferem os novos bindings.

Testes sintéticos do rastreador cobrem chamada/fallthrough, branches,
ciclos, instruções inalcançáveis, indiretos, interrupção por opcode,
endereços ausentes, acesso de segmento, duplicação e orçamento.

## Resultado e próxima dependência

Link real: relative=30885, local=1313, guest_bindings=683,
**host_bindings=65, pending=516**, tls_relocations=27. Na libc:
**45 referências pendentes a 44 símbolos distintos**.

Próximo trabalho concreto: fechar o subconjunto de inicialização única e
chaves TLS (Once/KeyCreate/SetSpecific), incluindo callbacks e seu lifetime,
e resolver os alvos indiretos dos caminhos relevantes. Por exemplo, o
grafo encontra um caminho do construtor 0x5d040 até a chamada de once em
0x5562f (PLT 0x380). Esse caminho inclui escolhas condicionais e não prova
que a chamada ocorra em todo bootstrap. Nenhum serviço foi substituído por
retorno fictício de sucesso para permitir execução prematura.

```bat
out\build\win-clang-debug\lbp3_host.exe --libc-environment-self-test
out\unfself\python\python.exe tools\trace_libc_startup.py --elf out\modules\lbp3\libc.elf --disassembly out\libc-full-disassembly.txt --audit analysis\libc\startup.json --output analysis\libc\reachability.json
ctest --preset win-clang-debug --output-on-failure
```

O disassembly completo é gerado por `llvm-objdump -d out/modules/lbp3/libc.elf`
com o LLVM instalado pelo Visual Studio. Ele fica em `out`, sem redistribuir
o ELF ou seu código no projeto. O JSON registra o hash do disassembly usado.
