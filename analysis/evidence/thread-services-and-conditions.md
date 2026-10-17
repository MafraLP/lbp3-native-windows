# Once, chaves, memória POSIX e condições: resultado integrado

Autorização: continuar implementação e testes autonomamente, registrando
falhas e limitações em `analysis/IMPLEMENTATION_LOG.md`.
Build clang-cl x64 e **19/19 conjuntos CTest passaram**. Nenhuma entrada ou
função da libc real foi executada. Callbacks exercitados são sintéticos.

## Inicialização única e chaves

Novos exports libkernel, biblioteca 1/módulo 0x0101, tipo função:

| Serviço | NID |
|---|---|
| scePthreadOnce | 14bOACANTBo |
| scePthreadKeyCreate | geDaqgH9lTg |
| scePthreadKeyDelete | PrdHuuDekhY |
| scePthreadSetspecific | +BzXYkqYeLE |
| scePthreadGetspecific | eoht7mQOCmo |

GuestOnce tem estado uint32 em offset 0, padding e slot de mutex em offset 8,
tamanho 16. Estado zero é não iniciado, um é concluído e dois em progresso.
Padding e slot de mutex são preservados. A biblioteca local usa o objeto em
0xcad38 e seu callback em 0x55dc0. A tabela host de inicializações em andamento
guarda owner e condição, sem colocar objetos C++ em memória guest. Chamadas
concorrentes aguardam o mesmo callback; o lock host é liberado durante sua
execução. Autorreentrada retorna EDEADLK como diagnóstico; estado inválido
ou em progresso sem registro host é recusado. Não há cancelamento, pthread_exit,
longjmp ou unwinding do callback. Apenas retorno normal foi implementado.

As 256 chaves usam índices int32. Cada slot tem geração interna: apagar e
recriar uma chave não torna visíveis valores antigos de outras threads.
O índice pode ser reutilizado, como nas APIs de chaves; um número antigo
não é um handle com geração exposta. Dados ficam em armazenamento por thread
host, independente dos templates PT_TLS. Contextos distintos têm IDs novos.

KeyDelete não chama destrutores. Na saída normal de GuestThread, a limpeza
ocorre antes do detach PT_TLS e antes de join completar. Limpa cada valor
antes do callback, libera o lock da tabela e permite novo SetSpecific.
Faz no máximo quatro passes; depois descarta os valores restantes. Não há
ordem de destrutores garantida para dependências entre chaves. O chamador
mantém código dos callbacks mapeado até todos os workers terminarem.

O escopo do controlador limpa seus próprios valores antes de remover a
tabela. A ordem no loader é threads -> chaves -> heap -> memória flexível
-> contexto TLS -> descarregamento (considerando destruição reversa dos
escopos). Um teste verifica que o destrutor do controlador ainda vê heap
registrado e TLS válido. Contextos concorrentes/nested são recusados.
Threads host externas só recebem limpeza se chamarem cleanup_thread_specific
explicitamente; não dependemos de destrutores thread_local para chamar código
guest depois que a infraestrutura já foi destruída.

## Callback da libc e mapa POSIX

Inspeção local: 0x5562f chama Once com callback 0x55dc0; em 0x55dd2 esse
callback cria uma chave, registrando destrutor 0x55df0. O pool em 0x580b0
chega ao mmap em 0x581af: seis argumentos SysV, addr=0, prot=3,
flags=0x1002 (private/anonymous), fd=-1, offset=0. Destrutor chama munmap
em 0x55e14. Adicionados esses callbacks como raízes explícitas ao rastreador;
isso resolve dois alvos conhecidos, não todos os indiretos.

Exports mmap `BPE9s9vQQXo` e munmap `UqDGjXA5yUM` em libkernel, mesmas
identidades acima. mmap suporta somente mapa anônimo/private, sem hint,
fd=-1, offset=0. Tamanho positivo é arredondado para 16 KiB com overflow
rejeitado. Usa o registro de propriedade/quota de memória flexível; proteções
0/1/3. Sucesso preserva errno. Falha retorna MAP_FAILED e escreve errno
positivo (não código SCE). munmap completo retorna zero ou -1 com errno;
arredonda comprimento como mmap. Arquivos, flags diferentes, endereço fixo,
GPU/execute e liberação parcial continuam recusados. Não são aliases para
as APIs SCE: os contratos de retorno e errno são distintos.

## Condition variables

| Serviço | NID |
|---|---|
| scePthreadCondInit | 2Tb92quprl0 |
| scePthreadCondDestroy | g+PZd2hiacg |
| scePthreadCondWait | WKAXJ4XBPQ4 |
| scePthreadCondSignal | kDh-NfxgMtE |
| scePthreadCondBroadcast | JGgj7Uvrl+A |

Até 128 condições, handles com tag e geração, inicializador estático zero e
tombstone dois. Atributos não nulos são recusados (ENOTSUP). CondTimedwait
continua **não resolvida**; não há fallback de sucesso para timed waits.

Wait enfileira um registro individual enquanto mantém o lock de condições,
depois libera o mutex guest. Signal/Broadcast usam o mesmo lock e marcam
waiters específicos: sinais não ficam armazenados para threads futuras.
Wait retoma o mutex antes de retornar e restaura profundidade recursiva.
Uma lease host mantém o mutex vivo e conta como waiter durante toda a
espera, impedindo destroy mesmo quando não há owner. Destroy da condição
também retorna EBUSY enquanto o waiter ainda está readquirindo o mutex.
Não há suporte a cancelamento, sinais, relógios, atributos ou layout privado
de condições acessado diretamente pelo guest. A condição pode sobreviver ao
escopo do loader se não for destruída explicitamente; ainda não há tabela
de condições/mutexes por processo guest. O host atual executa um conjunto
por vez e não executa código guest real. Encerrar/recarregar um processo
guest real exigirá limpeza explícita desses objetos restantes.

## Testes e limites

- Once com oito callers, uma execução, publicação de dados, recursão,
  controles inválidos/read-only, preservação dos campos adjacentes.
- Criação até 256 chaves, erros, reutilização, isolamento e quatro passes
  de destrutores que reinstalam valores. Workers criados/juntados pelas
  APIs scePthreadCreate/Join, com TLS acessível dentro dos destrutores.
- Controlador: limpeza enquanto heap e TLS ainda estão disponíveis.
- mmap: seis argumentos, zero fill, arredondamento, quota, MAP_FAILED,
  errno, falhas preservando mapas e isolamento de errno entre threads.
- Condições: signal, broadcast, inicialização lazy concorrente, ausência
  de perda de wakeup no handshake testado, retomada de mutex recursivo,
  ownership, EBUSY, stale handles, pointers read-only e limite de 128.
- Linker: todos os novos NIDs aplicados nas fixtures; suíte prévia preservada.

Contratos foram comparados às fontes locais auditadas do shadPS4, commit
`32021e41bc52f744c80d122e732e24e97496c98d` (`pthread.cpp`, `pthread_spec.cpp`,
`linker.h`) e ao disassembly da libc. Implementação própria, sem copiar
código externo. Estados/erros suportados não constituem validação em hardware.

## Resultado e bloqueios ainda abertos

Link: relative=30885, local=1313, guest_bindings=683, tls_relocations=27,
**host_bindings=88, pending=493**. Antes desta rodada: 65/516.
Libc: **33 referências a 32 símbolos distintos** (antes 45/44).

Grafo ampliado: 17.676 endereços, **11 imports pendentes potenciais**, 94
barreiras (83 indiretos, nove unknowns, dois endereços sem decode).
Pendências: debug exceptions, Write, signal, informações de módulo/unwind,
__pthread_cxa_finalize, _exit, sigfillset, sigprocmask e _is_signal_return.
O grafo explora ambos os lados de branches sem fluxo de dados e pode seguir
para dados após funções sem retorno; não prova que todos sejam necessários,
nem que só eles faltem. A análise continua incompleta; DT_INIT real não foi
chamado. Falhas de cancelamento, exceções e sinais não podem ser encobertas
por stubs de sucesso.

Validação repetível: `powershell -NoProfile -ExecutionPolicy Bypass -File
tools/validate.ps1`. O script registra automaticamente falhas de setup,
build ou CTest em `analysis/IMPLEMENTATION_LOG.md`, com etapa, código de saída
e saída do comando. Nenhum teste falhou nesta rodada; a questão de ordem de
limpeza identificada por revisão foi corrigida e ganhou teste integrado.
